/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_mediakeys_h__
#define mozilla_dom_mediakeys_h__

#include "nsIDOMMediaError.h"
#include "nsWrapperCache.h"
#include "nsISupports.h"
#include "mozilla/Attributes.h"
#include "mozilla/RefPtr.h"
#include "nsCOMPtr.h"
#include "nsCycleCollectionParticipant.h"
#include "nsRefPtrHashtable.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/MediaKeysBinding.h"
#include "mozIGeckoMediaPluginService.h"

namespace mozilla {

class CDMProxy;

namespace dom {

class ArrayBufferViewOrArrayBuffer;
class MediaKeySession;
class HTMLMediaElement;

typedef nsRefPtrHashtable<nsStringHashKey, MediaKeySession> KeySessionHashMap;
typedef nsRefPtrHashtable<nsUint32HashKey, dom::Promise> PromiseHashMap;
typedef nsRefPtrHashtable<nsUint32HashKey, MediaKeySession> PendingKeySessionsHashMap;
typedef uint32_t PromiseId;

// Helper function to extract data coming in from JS in an
// (ArrayBuffer or ArrayBufferView) IDL typed function argument.
bool
CopyArrayBufferViewOrArrayBufferData(const ArrayBufferViewOrArrayBuffer& aBufferOrView,
                                     nsTArray<uint8_t>& aOutData);

// This class is used on the main thread only.
// Note: it's addref/release is not (and can't be) thread safe!
class MediaKeys MOZ_FINAL : public nsISupports,
                            public nsWrapperCache
{
  ~MediaKeys();

public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(MediaKeys)

  MediaKeys(nsPIDOMWindow* aParentWindow, const nsAString& aKeySystem);

  nsPIDOMWindow* GetParentObject() const;

  virtual JSObject* WrapObject(JSContext* aCx) MOZ_OVERRIDE;

  nsresult Bind(HTMLMediaElement* aElement);

  // Javascript: readonly attribute DOMString keySystem;
  void GetKeySystem(nsString& retval) const;

  // JavaScript: MediaKeys.createSession()
  already_AddRefed<MediaKeySession> CreateSession(SessionType aSessionType,
                                                  ErrorResult& aRv);

  // JavaScript: MediaKeys.SetServerCertificate()
  already_AddRefed<Promise> SetServerCertificate(const ArrayBufferViewOrArrayBuffer& aServerCertificate,
                                                 ErrorResult& aRv);

  // JavaScript: MediaKeys.create()
  static
  already_AddRefed<Promise> Create(const GlobalObject& aGlobal,
                                   const nsAString& aKeySystem,
                                   ErrorResult& aRv);

  // JavaScript: MediaKeys.IsTypeSupported()
  static IsTypeSupportedResult IsTypeSupported(const GlobalObject& aGlobal,
                                               const nsAString& aKeySystem,
                                               const Optional<nsAString>& aInitDataType,
                                               const Optional<nsAString>& aContentType,
                                               const Optional<nsAString>& aCapability);

  already_AddRefed<MediaKeySession> GetSession(const nsAString& aSessionId);

  // Called once a Create() operation succeeds.
  void OnCDMCreated(PromiseId aId, const nsACString& aNodeId);
  // Called when GenerateRequest or Load have been called on a MediaKeySession
  // and we are waiting for its initialisation to finish.
  void OnSessionPending(PromiseId aId, MediaKeySession* aSession);
  // Called once a CreateSession succeeds.
  void OnSessionCreated(PromiseId aId, const nsAString& aSessionId);
  // Called once a LoadSession succeeds.
  void OnSessionLoaded(PromiseId aId, bool aSuccess);
  // Called once a session has closed.
  void OnSessionClosed(MediaKeySession* aSession);

  CDMProxy* GetCDMProxy() { return mProxy; }

  // Makes a new promise, or nullptr on failure.
  already_AddRefed<Promise> MakePromise(ErrorResult& aRv);
  // Stores promise in mPromises, returning an ID that can be used to retrieve
  // it later. The ID is passed to the CDM, so that it can signal specific
  // promises to be resolved.
  PromiseId StorePromise(Promise* aPromise);

  // Reject promise with DOMException corresponding to aExceptionCode.
  void RejectPromise(PromiseId aId, nsresult aExceptionCode);
  // Resolves promise with "undefined".
  void ResolvePromise(PromiseId aId);

  const nsCString& GetNodeId() const;

  void Shutdown();

  // Returns true if this MediaKeys has been bound to a media element.
  bool IsBoundToMediaElement() const;

  // Return NS_OK if the principals are the same as when the MediaKeys
  // was created, failure otherwise.
  nsresult CheckPrincipals();

  // Returns a pointer to the bound media element's owner doc.
  // If we're not bound, this returns null.
  nsIDocument* GetOwnerDoc() const;

private:

  static bool IsTypeSupported(const nsAString& aKeySystem,
                              const Optional<nsAString>& aInitDataType = Optional<nsAString>(),
                              const Optional<nsAString>& aContentType = Optional<nsAString>());

  bool IsInPrivateBrowsing();
  already_AddRefed<Promise> Init(ErrorResult& aRv);

  // Removes promise from mPromises, and returns it.
  already_AddRefed<Promise> RetrievePromise(PromiseId aId);

  // Owning ref to proxy. The proxy has a weak reference back to the MediaKeys,
  // and the MediaKeys destructor clears the proxy's reference to the MediaKeys.
  nsRefPtr<CDMProxy> mProxy;

  nsRefPtr<HTMLMediaElement> mElement;

  nsCOMPtr<nsPIDOMWindow> mParent;
  nsString mKeySystem;
  nsCString mNodeId;
  KeySessionHashMap mKeySessions;
  PromiseHashMap mPromises;
  PendingKeySessionsHashMap mPendingSessions;
  PromiseId mCreatePromiseId;

  nsRefPtr<nsIPrincipal> mPrincipal;
  nsRefPtr<nsIPrincipal> mTopLevelPrincipal;

};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_mediakeys_h__
