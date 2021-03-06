/*
 * Copyright 2016-2017 TATA ELXSI
 * Copyright 2016-2017 Metrological
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "MediaSession.h"
#include <assert.h>
#include <iostream>
#include <sstream>
#include <string>
#include <string.h>
#include <vector>
#include <sys/utsname.h>
#include <drmerror.h>

#define NYI_KEYSYSTEM "keysystem-placeholder"

using namespace std;

namespace CDMi {

// The default location of CDM DRM store.
// /tmp/drmstore.dat
#ifdef PR_3_3
const DRM_WCHAR g_rgwchCDMDrmStoreName[] = {DRM_WCHAR_CAST('/'), DRM_WCHAR_CAST('t'), DRM_WCHAR_CAST('m'), DRM_WCHAR_CAST('p'), DRM_WCHAR_CAST('/'),
                                            DRM_WCHAR_CAST('d'), DRM_WCHAR_CAST('r'), DRM_WCHAR_CAST('m'), DRM_WCHAR_CAST('s'), DRM_WCHAR_CAST('t'),
                                            DRM_WCHAR_CAST('o'), DRM_WCHAR_CAST('r'), DRM_WCHAR_CAST('e'), DRM_WCHAR_CAST('.'), DRM_WCHAR_CAST('d'),
                                            DRM_WCHAR_CAST('a'), DRM_WCHAR_CAST('t'), DRM_WCHAR_CAST('\0')};

const DRM_CONST_STRING g_dstrCDMDrmStoreName = DRM_CREATE_DRM_STRING(g_rgwchCDMDrmStoreName);
#else
const DRM_WCHAR g_rgwchCDMDrmStoreName[] = {WCHAR_CAST('/'), WCHAR_CAST('t'), WCHAR_CAST('m'), WCHAR_CAST('p'), WCHAR_CAST('/'),
                                            WCHAR_CAST('d'), WCHAR_CAST('r'), WCHAR_CAST('m'), WCHAR_CAST('s'), WCHAR_CAST('t'),
                                            WCHAR_CAST('o'), WCHAR_CAST('r'), WCHAR_CAST('e'), WCHAR_CAST('.'), WCHAR_CAST('d'),
                                            WCHAR_CAST('a'), WCHAR_CAST('t'), WCHAR_CAST('\0')};

const DRM_CONST_STRING g_dstrCDMDrmStoreName = CREATE_DRM_STRING(g_rgwchCDMDrmStoreName);
#endif

const DRM_CONST_STRING *g_rgpdstrRights[1] = {&g_dstrWMDRM_RIGHT_PLAYBACK};


// Parse out the first PlayReady initialization header found in the concatenated
// block of headers in _initData_.
// If a PlayReady header is found, this function returns true and the header
// contents are stored in _output_.
// Otherwise, returns false and _output_ is not touched.
bool parsePlayreadyInitializationData(const std::string& initData, std::string* output)
{
    BufferReader input(reinterpret_cast<const uint8_t*>(initData.data()), initData.length());

    static const uint8_t playreadySystemId[] = {
      0x9A, 0x04, 0xF0, 0x79, 0x98, 0x40, 0x42, 0x86,
      0xAB, 0x92, 0xE6, 0x5B, 0xE0, 0x88, 0x5F, 0x95,
    };

    // one PSSH box consists of:
    // 4 byte size of the atom, inclusive.  (0 means the rest of the buffer.)
    // 4 byte atom type, "pssh".
    // (optional, if size == 1) 8 byte size of the atom, inclusive.
    // 1 byte version, value 0 or 1.  (skip if larger.)
    // 3 byte flags, value 0.  (ignored.)
    // 16 byte system id.
    // (optional, if version == 1) 4 byte key ID count. (K)
    // (optional, if version == 1) K * 16 byte key ID.
    // 4 byte size of PSSH data, exclusive. (N)
    // N byte PSSH data.
    while (!input.IsEOF()) {
      size_t startPosition = input.pos();

      // The atom size, used for skipping.
      uint64_t atomSize;

      if (!input.Read4Into8(&atomSize)) {
        return false;
      }

      std::vector<uint8_t> atomType;
      if (!input.ReadVec(&atomType, 4)) {
          return false;
      }

      if (atomSize == 1) {
          if (!input.Read8(&atomSize)) {
              return false;
          }
      } else if (atomSize == 0) {
        atomSize = input.size() - startPosition;
      }

      if (memcmp(&atomType[0], "pssh", 4)) {
          if (!input.SkipBytes(atomSize - (input.pos() - startPosition))) {
            return false;
          }
          continue;
      }

      uint8_t version;
      if (!input.Read1(&version)) {
          return false;
      }


      if (version > 1) {
        // unrecognized version - skip.
        if (!input.SkipBytes(atomSize - (input.pos() - startPosition))) {
          return false;
        }
        continue;
      }

      // flags
      if (!input.SkipBytes(3)) {
        return false;
      }

      // system id
      std::vector<uint8_t> systemId;
      if (!input.ReadVec(&systemId, sizeof(playreadySystemId))) {
        return false;
      }

      if (memcmp(&systemId[0], playreadySystemId, sizeof(playreadySystemId))) {
        // skip non-Playready PSSH boxes.
        if (!input.SkipBytes(atomSize - (input.pos() - startPosition))) {
          return false;
        }
        continue;
      }

      if (version == 1) {
        // v1 has additional fields for key IDs.  We can skip them.
        uint32_t numKeyIds;
        if (!input.Read4(&numKeyIds)) {
          return false;
        }

        if (!input.SkipBytes(numKeyIds * 16)) {
          return false;
        }
      }

      // size of PSSH data
      uint32_t dataLength;
      if (!input.Read4(&dataLength)) {
        return false;
      }

      output->clear();
      if (!input.ReadString(output, dataLength)) {
        return false;
      }

      return true;
  }

  // we did not find a matching record
  return false;
}

MediaKeySession::MediaKeySession(const uint8_t *f_pbInitData, uint32_t f_cbInitData)
    : m_poAppContext(nullptr)
    , m_pbOpaqueBuffer(nullptr) 
    , m_cbOpaqueBuffer(0)
    , m_pbRevocationBuffer(nullptr)
    , m_eKeyState(KEY_INIT)
    , m_fCommit(FALSE)
    , m_pbChallenge(nullptr)
    , m_cbChallenge(0)
    , m_pchSilentURL(nullptr) 
    , m_piCallback(nullptr) {
  DRM_RESULT dr = DRM_SUCCESS;
  DRM_ID oSessionID;

#ifdef PR_3_3
  DRM_DWORD cchEncodedSessionID = sizeof(m_rgchSessionID);
#else
  DRM_DWORD cchEncodedSessionID = SIZEOF(m_rgchSessionID);
#endif
  // FIXME: Change the interface of this method? Not sure why the win32 bondage is still so popular.
  std::string initData(reinterpret_cast<const char*>(f_pbInitData), f_cbInitData);
  std::string playreadyInitData;

  printf("Constructing PlayReady Session [%p]\n", this);

  ChkMem(m_pbOpaqueBuffer = (DRM_BYTE *)Oem_MemAlloc(MINIMUM_APPCONTEXT_OPAQUE_BUFFER_SIZE));
  m_cbOpaqueBuffer = MINIMUM_APPCONTEXT_OPAQUE_BUFFER_SIZE;

#ifdef PR_3_3
  ChkMem(m_poAppContext = (DRM_APP_CONTEXT *)Oem_MemAlloc(sizeof(DRM_APP_CONTEXT)));
#else
  ChkMem(m_poAppContext = (DRM_APP_CONTEXT *)Oem_MemAlloc(SIZEOF(DRM_APP_CONTEXT)));
#endif

  // Initialize DRM app context.
  ChkDR(Drm_Initialize(m_poAppContext,
                       NULL,
                       m_pbOpaqueBuffer,
                       m_cbOpaqueBuffer,
                       &g_dstrCDMDrmStoreName));

  if (DRM_REVOCATION_IsRevocationSupported()) {
    ChkMem(m_pbRevocationBuffer = (DRM_BYTE *)Oem_MemAlloc(REVOCATION_BUFFER_SIZE));

    ChkDR(Drm_Revocation_SetBuffer(m_poAppContext,
                                   m_pbRevocationBuffer,
                                   REVOCATION_BUFFER_SIZE));
  }

#ifdef PR_3_3
  // Generate a random media session ID.
  ChkDR(Oem_Random_GetBytes(NULL, (DRM_BYTE *)&oSessionID, sizeof(oSessionID)));
  ZEROMEM(m_rgchSessionID, sizeof(m_rgchSessionID));

  // Store the generated media session ID in base64 encoded form.
  ChkDR(DRM_B64_EncodeA((DRM_BYTE *)&oSessionID,
                        sizeof(oSessionID),
                        m_rgchSessionID,
                        &cchEncodedSessionID,
                        0));
#else
  // Generate a random media session ID.
  ChkDR(Oem_Random_GetBytes(NULL, (DRM_BYTE *)&oSessionID, SIZEOF(oSessionID)));
  ZEROMEM(m_rgchSessionID, SIZEOF(m_rgchSessionID));

  // Store the generated media session ID in base64 encoded form.
  ChkDR(DRM_B64_EncodeA((DRM_BYTE *)&oSessionID,
                        SIZEOF(oSessionID),
                        m_rgchSessionID,
                        &cchEncodedSessionID,
                        0));
#endif


  // The current state MUST be KEY_INIT otherwise error out.
  ChkBOOL(m_eKeyState == KEY_INIT, DRM_E_INVALIDARG);

  if (!parsePlayreadyInitializationData(initData, &playreadyInitData)) {
      playreadyInitData = initData;
  }
  ChkDR(Drm_Content_SetProperty(m_poAppContext,
                                DRM_CSP_AUTODETECT_HEADER,
                                reinterpret_cast<const DRM_BYTE*>(playreadyInitData.data()),
                                playreadyInitData.size()));

  // The current state MUST be KEY_INIT otherwise error out.
  ChkBOOL(m_eKeyState == KEY_INIT, DRM_E_INVALIDARG);
  return; 

ErrorExit:
  if (DRM_FAILED(dr)) {
    const DRM_CHAR* description;
    DRM_ERR_GetErrorNameFromCode(dr, &description);
    printf("playready error: %s\n", description);
  }
}

MediaKeySession::~MediaKeySession(void) {

  Drm_Uninitialize(m_poAppContext);
 
  SAFE_OEM_FREE(m_pbChallenge);
  SAFE_OEM_FREE(m_pchSilentURL);

  if (DRM_REVOCATION_IsRevocationSupported())
    SAFE_OEM_FREE(m_pbRevocationBuffer);

  SAFE_OEM_FREE(m_pbOpaqueBuffer);
  SAFE_OEM_FREE(m_poAppContext);

  m_eKeyState = KEY_CLOSED;
  printf("Destructing PlayReady Session [%p]\n", this);
}

const char *MediaKeySession::GetSessionId(void) const {
  return m_rgchSessionID;
}

const char *MediaKeySession::GetKeySystem(void) const {
  return NYI_KEYSYSTEM; // FIXME : replace with keysystem and test.
}

DRM_RESULT DRM_CALL MediaKeySession::_PolicyCallback(
    const DRM_VOID *f_pvOutputLevelsData, 
    DRM_POLICY_CALLBACK_TYPE f_dwCallbackType,
#ifdef PR_3_3
    const DRM_KID *f_pKID,
    const DRM_LID *f_pLID,
#endif
    const DRM_VOID *f_pv) {
  return DRM_SUCCESS;
}

void MediaKeySession::Run(const IMediaKeySessionCallback *f_piMediaKeySessionCallback) {
   
  if (f_piMediaKeySessionCallback) {
    m_piCallback = const_cast<IMediaKeySessionCallback *>(f_piMediaKeySessionCallback);

    // FIXME : Custom data is not set;needs recheck.
    playreadyGenerateKeyRequest();
  } else {
      m_piCallback = nullptr;
  }
}

bool MediaKeySession::playreadyGenerateKeyRequest() {
    
  DRM_RESULT dr = DRM_SUCCESS; 
  DRM_DWORD cchSilentURL = 0;
#ifdef PR_3_3
  DRM_ANSI_STRING dastrCustomData = DRM_EMPTY_DRM_STRING;
#else
  DRM_ANSI_STRING dastrCustomData = EMPTY_DRM_STRING;
#endif

/* PRv3.3 support */
#ifdef PR_3_3
  dr = Drm_Reader_Bind(m_poAppContext,
                        g_rgpdstrRights,
                        DRM_NO_OF(g_rgpdstrRights),
                        _PolicyCallback,
                        NULL,
                        &m_oDecryptContext);

  if (dr == DRM_SUCCESS)
  {
      m_eKeyState = KEY_READY;
      if (m_piCallback)
        m_piCallback->OnKeyStatusUpdate("KeyUsable", nullptr, 0);
      return true;
  }
#endif

  // FIXME :  Check add case Play rights already acquired
  // Try to figure out the size of the license acquisition
  // challenge to be returned.
  dr = Drm_LicenseAcq_GenerateChallenge(m_poAppContext,
                                        g_rgpdstrRights,
                                        sizeof(g_rgpdstrRights) / sizeof(DRM_CONST_STRING *),
                                         NULL,
                                         NULL, // FIXME : Custom data
                                         0, // FIXME : Custon data size 
                                         NULL,
                                         &cchSilentURL,
                                         NULL,
                                         NULL,
#ifdef PR_3_3						//PRv3.3 support
                                         m_pbChallenge,
                                         &m_cbChallenge,
                                         NULL);
#else
                                         NULL,
                                         &m_cbChallenge);
#endif

  if (dr == DRM_E_BUFFERTOOSMALL) {
    if (cchSilentURL > 0) {
      ChkMem(m_pchSilentURL = (DRM_CHAR *)Oem_MemAlloc(cchSilentURL + 1));
      ZEROMEM(m_pchSilentURL, cchSilentURL + 1);
    }

    // Allocate buffer that is sufficient to store the license acquisition
    // challenge.
    if (m_cbChallenge > 0)
      ChkMem(m_pbChallenge = (DRM_BYTE *)Oem_MemAlloc(m_cbChallenge));

    dr = DRM_SUCCESS;
  } else {
    ChkDR(dr);
  }

  // Supply a buffer to receive the license acquisition challenge.
  ChkDR(Drm_LicenseAcq_GenerateChallenge(m_poAppContext,
                                         g_rgpdstrRights,
                                         sizeof(g_rgpdstrRights) / sizeof(DRM_CONST_STRING *),
                                         NULL,
                                         NULL, // FIXME : Custom data
                                         0, // FIXME : Custon data size 
                                         m_pchSilentURL,
                                         &cchSilentURL,
                                         NULL,
                                         NULL,
                                         m_pbChallenge,
#ifdef PR_3_3						//PRv3.3 support
                                         &m_cbChallenge,
                                         NULL));
#else
                                         &m_cbChallenge));
#endif


  m_eKeyState = KEY_PENDING;
  if (m_piCallback)
        m_piCallback->OnKeyMessage((const uint8_t *) m_pbChallenge, m_cbChallenge, (char *)m_pchSilentURL);
  return true;

ErrorExit:
  if (DRM_FAILED(dr)) {
    const DRM_CHAR* description;
    DRM_ERR_GetErrorNameFromCode(dr, &description);
    printf("playready error: %s\n", description);
  }
  return false;
}

CDMi_RESULT MediaKeySession::Load(void) {
  return CDMi_S_FALSE;
}

void MediaKeySession::Update(const uint8_t *m_pbKeyMessageResponse, uint32_t  m_cbKeyMessageResponse) {

  DRM_RESULT dr = DRM_SUCCESS;
  DRM_LICENSE_RESPONSE oLicenseResponse = {eUnknownProtocol, 0};

  // The current state MUST be KEY_PENDING otherwise error out.
  ChkBOOL(m_eKeyState == KEY_PENDING, DRM_E_INVALIDARG);

  ChkArg(m_pbKeyMessageResponse && m_cbKeyMessageResponse > 0);

  ChkDR(Drm_LicenseAcq_ProcessResponse(m_poAppContext,
                                       DRM_PROCESS_LIC_RESPONSE_SIGNATURE_NOT_REQUIRED,
#ifndef PR_3_3				//PRv3.3 support
                                       NULL,
                                       NULL,
#endif
                                       const_cast<DRM_BYTE *>(m_pbKeyMessageResponse),
                                       m_cbKeyMessageResponse,
                                       &oLicenseResponse));

#ifdef PR_3_3
  ChkDR(Drm_Reader_Bind(m_poAppContext,
                        g_rgpdstrRights,
                        DRM_NO_OF(g_rgpdstrRights),
                        _PolicyCallback,
                        NULL,
                        &m_oDecryptContext));
#else
  ChkDR(Drm_Reader_Bind(m_poAppContext,
                        g_rgpdstrRights,
                        NO_OF(g_rgpdstrRights),
                        _PolicyCallback,
                        NULL,
                        &m_oDecryptContext));
#endif

  m_eKeyState = KEY_READY;

  if (m_eKeyState == KEY_READY) {
      if (m_piCallback)
        m_piCallback->OnKeyStatusUpdate("KeyUsable", nullptr, 0);
  }
  return;

ErrorExit:
  if (DRM_FAILED(dr)) {
    const DRM_CHAR* description;
    DRM_ERR_GetErrorNameFromCode(dr, &description);
    printf("playready error: %s\n", description);

    m_eKeyState = KEY_ERROR;
  }
  return;
}

CDMi_RESULT MediaKeySession::Remove(void) {
  return CDMi_S_FALSE;
}

CDMi_RESULT MediaKeySession::Close(void) {}

CDMi_RESULT MediaKeySession::Decrypt(
    const uint8_t *f_pbSessionKey,
    uint32_t f_cbSessionKey,
    const uint32_t *f_pdwSubSampleMapping,
    uint32_t f_cdwSubSampleMapping,
    const uint8_t *f_pbIV,
    uint32_t f_cbIV,
    const uint8_t *payloadData,
    uint32_t payloadDataSize,
    uint32_t *f_pcbOpaqueClearContent,
    uint8_t **f_ppbOpaqueClearContent,
    const uint8_t /* keyIdLength */,
    const uint8_t* /* keyId */)
{
  CDMi_RESULT status = CDMi_S_FALSE;
  DRM_AES_COUNTER_MODE_CONTEXT oAESContext = {0};
  DRM_RESULT dr = DRM_SUCCESS;

  uint8_t *ivData = (uint8_t *) f_pbIV;
  uint8_t temp;

/* PRv3.3 support */
#ifdef PR_3_3
    DRM_DWORD rgdwMappings[2];
    if( f_pcbOpaqueClearContent == NULL || f_ppbOpaqueClearContent == NULL )
    {
        dr = DRM_E_INVALIDARG;
        goto ErrorExit;
    }

    *f_pcbOpaqueClearContent = 0;
    *f_ppbOpaqueClearContent = NULL;

    ChkBOOL(m_eKeyState == KEY_READY, DRM_E_INVALIDARG);
    ChkArg(f_pbIV != NULL && f_cbIV == sizeof(DRM_UINT64));
#else
  ChkDR(Drm_Reader_InitDecrypt(&m_oDecryptContext, NULL, 0));
#endif

  // FIXME: IV bytes need to be swapped ???
  for (uint32_t i = 0; i < f_cbIV / 2; i++) {
    temp = ivData[i];
    ivData[i] = ivData[f_cbIV - i - 1];
    ivData[f_cbIV - i - 1] = temp;
  }

  MEMCPY(&oAESContext.qwInitializationVector, ivData, f_cbIV);

/* PRv3.3 support */
#ifdef PR_3_3
    if ( NULL == f_pdwSubSampleMapping )
    {
        rgdwMappings[0] = 0;
        rgdwMappings[1] = payloadDataSize;
        f_pdwSubSampleMapping = reinterpret_cast<const uint32_t*>(rgdwMappings);
        f_cdwSubSampleMapping = DRM_NO_OF(rgdwMappings);
    }

    ChkDR(Drm_Reader_DecryptOpaque(
        &m_oDecryptContext,
        f_cdwSubSampleMapping,
        reinterpret_cast<const DRM_DWORD*>(f_pdwSubSampleMapping),
        oAESContext.qwInitializationVector,
        payloadDataSize,
        (DRM_BYTE *) payloadData,
        reinterpret_cast<DRM_DWORD*>(f_pcbOpaqueClearContent),
        reinterpret_cast<DRM_BYTE**>(f_ppbOpaqueClearContent)));
#else
  ChkDR(Drm_Reader_Decrypt(&m_oDecryptContext, &oAESContext, (DRM_BYTE *) payloadData,  payloadDataSize));
#endif
     
  // Call commit during the decryption of the first sample.
  if (!m_fCommit) {
    ChkDR(Drm_Reader_Commit(m_poAppContext, _PolicyCallback, NULL));
    m_fCommit = TRUE;
  } 

/* PRv3.3 support */
#ifndef PR_3_3
  // Return clear content.
  *f_pcbOpaqueClearContent = payloadDataSize;
  *f_ppbOpaqueClearContent = (uint8_t *)payloadData;
#endif
  status = CDMi_SUCCESS;

  return status;

ErrorExit:
  if (DRM_FAILED(dr)) {
    const DRM_CHAR* description;
    DRM_ERR_GetErrorNameFromCode(dr, &description);
    printf("playready error: %s\n", description);

/* PRv3.3 support */
#ifdef PR_3_3
        if( f_pcbOpaqueClearContent != NULL )
          {
              *f_pcbOpaqueClearContent = 0;
          }
          if( f_ppbOpaqueClearContent != NULL )
          {
              *f_ppbOpaqueClearContent = NULL;
          }
#endif
  }
  return status;
}

CDMi_RESULT MediaKeySession::ReleaseClearContent(
    const uint8_t *f_pbSessionKey,
    uint32_t f_cbSessionKey,
    const uint32_t  f_cbClearContentOpaque,
    uint8_t  *f_pbClearContentOpaque ) {

  return CDMi_SUCCESS;

}

}  // namespace CDMi
