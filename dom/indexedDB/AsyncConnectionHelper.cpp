/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "AsyncConnectionHelper.h"

#include "mozilla/dom/quota/QuotaManager.h"
#include "mozilla/storage.h"
#include "nsComponentManagerUtils.h"
#include "nsContentUtils.h"
#include "nsProxyRelease.h"
#include "nsThreadUtils.h"
#include "nsWrapperCacheInlines.h"

#include "FileInfo.h"
#include "IDBEvents.h"
#include "IDBObjectStore.h"
#include "IDBTransaction.h"
#include "IndexedDatabaseManager.h"
#include "ProfilerHelpers.h"
#include "ReportInternalError.h"
#include "TransactionThreadPool.h"

using namespace mozilla;
USING_INDEXEDDB_NAMESPACE
using mozilla::dom::quota::QuotaManager;

namespace {

IDBTransaction* gCurrentTransaction = nullptr;

const uint32_t kProgressHandlerGranularity = 1000;

class MOZ_STACK_CLASS TransactionPoolEventTarget : public StackBasedEventTarget
{
public:
  NS_DECL_NSIEVENTTARGET

  TransactionPoolEventTarget(IDBTransaction* aTransaction)
  : mTransaction(aTransaction)
  { }

private:
  IDBTransaction* mTransaction;
};

// This inline is just so that we always clear aBuffers appropriately even if
// something fails.
inline
nsresult
ConvertCloneReadInfosToArrayInternal(
                                JSContext* aCx,
                                nsTArray<StructuredCloneReadInfo>& aReadInfos,
                                JS::MutableHandle<JS::Value> aResult)
{
  JS::Rooted<JSObject*> array(aCx, JS_NewArrayObject(aCx, 0));
  if (!array) {
    IDB_WARNING("Failed to make array!");
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  if (!aReadInfos.IsEmpty()) {
    if (!JS_SetArrayLength(aCx, array, uint32_t(aReadInfos.Length()))) {
      IDB_WARNING("Failed to set array length!");
      return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
    }

    for (uint32_t index = 0, count = aReadInfos.Length(); index < count;
         index++) {
      StructuredCloneReadInfo& readInfo = aReadInfos[index];

      JS::Rooted<JS::Value> val(aCx);
      if (!IDBObjectStore::DeserializeValue(aCx, readInfo, &val)) {
        NS_WARNING("Failed to decode!");
        return NS_ERROR_DOM_DATA_CLONE_ERR;
      }

      if (!JS_SetElement(aCx, array, index, val)) {
        IDB_WARNING("Failed to set array element!");
        return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
      }
    }
  }

  aResult.setObject(*array);
  return NS_OK;
}

} // anonymous namespace

HelperBase::~HelperBase()
{
  if (!NS_IsMainThread()) {
    IDBRequest* request;
    mRequest.forget(&request);

    if (request) {
      nsCOMPtr<nsIThread> mainThread;
      NS_GetMainThread(getter_AddRefs(mainThread));
      NS_WARN_IF_FALSE(mainThread, "Couldn't get the main thread!");

      if (mainThread) {
        NS_ProxyRelease(mainThread, static_cast<EventTarget*>(request));
      }
    }
  }
}

nsresult
HelperBase::WrapNative(JSContext* aCx,
                       nsISupports* aNative,
                       JS::MutableHandle<JS::Value> aResult)
{
  NS_ASSERTION(aCx, "Null context!");
  NS_ASSERTION(aNative, "Null pointer!");
  NS_ASSERTION(aResult.address(), "Null pointer!");
  NS_ASSERTION(mRequest, "Null request!");

  nsRefPtr<IDBWrapperCache> wrapper = static_cast<IDBWrapperCache*>(mRequest);
  JS::Rooted<JSObject*> global(aCx, wrapper->GetParentObject());
  NS_ASSERTION(global, "This should never be null!");

  nsresult rv =
    nsContentUtils::WrapNative(aCx, global, aNative, aResult);
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  return NS_OK;
}

void
HelperBase::ReleaseMainThreadObjects()
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  mRequest = nullptr;
}

AsyncConnectionHelper::AsyncConnectionHelper(IDBDatabase* aDatabase,
                                             IDBRequest* aRequest)
: HelperBase(aRequest),
  mDatabase(aDatabase),
  mResultCode(NS_OK),
  mDispatched(false)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
}

AsyncConnectionHelper::AsyncConnectionHelper(IDBTransaction* aTransaction,
                                             IDBRequest* aRequest)
: HelperBase(aRequest),
  //mDatabase(aTransaction->mDatabase),
  mTransaction(aTransaction),
  mResultCode(NS_OK),
  mDispatched(false)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
}

AsyncConnectionHelper::~AsyncConnectionHelper()
{
  if (!NS_IsMainThread()) {
    IDBDatabase* database;
    mDatabase.forget(&database);

    IDBTransaction* transaction;
    mTransaction.forget(&transaction);

    nsCOMPtr<nsIThread> mainThread;
    NS_GetMainThread(getter_AddRefs(mainThread));
    NS_WARN_IF_FALSE(mainThread, "Couldn't get the main thread!");

    if (mainThread) {
      if (database) {
        NS_ProxyRelease(mainThread, static_cast<IDBWrapperCache*>(database));
      }
      if (transaction) {
        NS_ProxyRelease(mainThread, static_cast<IDBWrapperCache*>(transaction));
      }
    }
  }

  NS_ASSERTION(!mOldProgressHandler, "Should not have anything here!");
}

NS_IMPL_ISUPPORTS2(AsyncConnectionHelper, nsIRunnable,
                   mozIStorageProgressHandler)

NS_IMETHODIMP
AsyncConnectionHelper::Run()
{
  MOZ_CRASH("Remove me!");
}

NS_IMETHODIMP
AsyncConnectionHelper::OnProgress(mozIStorageConnection* aConnection,
                                  bool* _retval)
{
  if (mDatabase && mDatabase->IsInvalidated()) {
    // Someone is trying to delete the database file. Exit lightningfast!
    *_retval = true;
    return NS_OK;
  }

  if (mOldProgressHandler) {
    return mOldProgressHandler->OnProgress(aConnection, _retval);
  }

  *_retval = false;
  return NS_OK;
}

nsresult
AsyncConnectionHelper::Dispatch(nsIEventTarget* aTarget)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  nsresult rv = Init();
  if (NS_FAILED(rv)) {
    return rv;
  }

  rv = aTarget->Dispatch(this, NS_DISPATCH_NORMAL);
  NS_ENSURE_SUCCESS(rv, rv);

  if (mTransaction) {
    mTransaction->OnNewRequest();
  }

  mDispatched = true;

  return NS_OK;
}

nsresult
AsyncConnectionHelper::DispatchToTransactionPool()
{
  NS_ASSERTION(mTransaction, "Only ok to call this with a transaction!");
  TransactionPoolEventTarget target(mTransaction);
  return Dispatch(&target);
}

// static
void
AsyncConnectionHelper::SetCurrentTransaction(IDBTransaction* aTransaction)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(!aTransaction || !gCurrentTransaction,
               "Stepping on another transaction!");

  gCurrentTransaction = aTransaction;
}

// static
IDBTransaction*
AsyncConnectionHelper::GetCurrentTransaction()
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  return gCurrentTransaction;
}

nsresult
AsyncConnectionHelper::Init()
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  return NS_OK;
}

already_AddRefed<nsIDOMEvent>
AsyncConnectionHelper::CreateSuccessEvent(mozilla::dom::EventTarget* aOwner)
{
  return CreateGenericEvent(mRequest, NS_LITERAL_STRING(SUCCESS_EVT_STR),
                            eDoesNotBubble, eNotCancelable);
}

nsresult
AsyncConnectionHelper::OnSuccess()
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(mRequest, "Null request!");

  PROFILER_MAIN_THREAD_LABEL("IndexedDB", "AsyncConnectionHelper::OnSuccess");

  nsRefPtr<nsIDOMEvent> event = CreateSuccessEvent(mRequest);
  if (!event) {
    IDB_WARNING("Failed to create event!");
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  bool dummy;
  nsresult rv = mRequest->DispatchEvent(event, &dummy);
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  WidgetEvent* internalEvent = event->GetInternalNSEvent();
  NS_ASSERTION(internalEvent, "This should never be null!");

  NS_ASSERTION(!mTransaction ||
               mTransaction->IsOpen() ||
               mTransaction->IsAborted(),
               "How else can this be closed?!");

  if (internalEvent->mFlags.mExceptionHasBeenRisen &&
      mTransaction &&
      mTransaction->IsOpen()) {
    rv = mTransaction->Abort(NS_ERROR_DOM_INDEXEDDB_ABORT_ERR);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  return NS_OK;
}

void
AsyncConnectionHelper::OnError()
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(mRequest, "Null request!");

  PROFILER_MAIN_THREAD_LABEL("IndexedDB", "AsyncConnectionHelper::OnError");

  // Make an error event and fire it at the target.
  nsRefPtr<nsIDOMEvent> event =
    CreateGenericEvent(mRequest, NS_LITERAL_STRING(ERROR_EVT_STR), eDoesBubble,
                       eCancelable);
  if (!event) {
    NS_ERROR("Failed to create event!");
    return;
  }

  bool doDefault;
  nsresult rv = mRequest->DispatchEvent(event, &doDefault);
  if (NS_SUCCEEDED(rv)) {
    NS_ASSERTION(!mTransaction ||
                 mTransaction->IsOpen() ||
                 mTransaction->IsAborted(),
                 "How else can this be closed?!");

    WidgetEvent* internalEvent = event->GetInternalNSEvent();
    NS_ASSERTION(internalEvent, "This should never be null!");

    if (internalEvent->mFlags.mExceptionHasBeenRisen &&
        mTransaction &&
        mTransaction->IsOpen() &&
        NS_FAILED(mTransaction->Abort(NS_ERROR_DOM_INDEXEDDB_ABORT_ERR))) {
      NS_WARNING("Failed to abort transaction!");
    }

    if (doDefault &&
        mTransaction &&
        mTransaction->IsOpen() &&
        NS_FAILED(mTransaction->Abort(mRequest))) {
      NS_WARNING("Failed to abort transaction!");
    }
  }
  else {
    NS_WARNING("DispatchEvent failed!");
  }
}

nsresult
AsyncConnectionHelper::GetSuccessResult(JSContext* aCx,
                                        JS::MutableHandle<JS::Value> aVal)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  aVal.setUndefined();
  return NS_OK;
}

void
AsyncConnectionHelper::ReleaseMainThreadObjects()
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  mDatabase = nullptr;
  mTransaction = nullptr;

  HelperBase::ReleaseMainThreadObjects();
}

AsyncConnectionHelper::ChildProcessSendResult
AsyncConnectionHelper::MaybeSendResponseToChildProcess(nsresult aResultCode)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");

  // If there's no request, there could never have been an actor, and so there
  // is nothing to do.
  if (!mRequest) {
    return Success_NotSent;
  }

  IDBTransaction* trans = GetCurrentTransaction();
  // We may not have a transaction, e.g. for deleteDatabase
  if (!trans) {
    return Success_NotSent;
  }

  MOZ_CRASH("Remove me!");
  /*
  // Are we shutting down the child?
  IndexedDBDatabaseParent* dbActor = trans->Database()->GetActorParent();
  if (dbActor && dbActor->IsDisconnected()) {
    return Success_ActorDisconnected;
  }

  IndexedDBRequestParentBase* actor = mRequest->GetActorParent();
  if (!actor) {
    return Success_NotSent;
  }*/

  IDB_PROFILER_MARK("IndexedDB Request %llu: Sending response to child "
                    "process (rv = %lu)",
                    "IDBRequest[%llu] MT Done",
                    mRequest->GetSerialNumber(), aResultCode);

  return SendResponseToChildProcess(aResultCode);
}

nsresult
AsyncConnectionHelper::OnParentProcessRequestComplete(
                                            const ResponseValue& aResponseValue)
{
  NS_ASSERTION(!IndexedDatabaseManager::IsMainProcess(), "Wrong process!");

  MOZ_CRASH("Remove me!");
/*
  if (aResponseValue.type() == ResponseValue::Tnsresult) {
    NS_ASSERTION(NS_FAILED(aResponseValue.get_nsresult()), "Huh?");
    SetError(aResponseValue.get_nsresult());
  }
  else {
    nsresult rv = UnpackResponseFromParentProcess(aResponseValue);
    NS_ENSURE_SUCCESS(rv, rv);
  }
  */
  return Run();
}

// static
nsresult
AsyncConnectionHelper::ConvertToArrayAndCleanup(
                                  JSContext* aCx,
                                  nsTArray<StructuredCloneReadInfo>& aReadInfos,
                                  JS::MutableHandle<JS::Value> aResult)
{
  NS_ASSERTION(aCx, "Null context!");
  NS_ASSERTION(aResult.address(), "Null pointer!");

  nsresult rv = ConvertCloneReadInfosToArrayInternal(aCx, aReadInfos, aResult);

  for (uint32_t index = 0; index < aReadInfos.Length(); index++) {
    aReadInfos[index].mCloneBuffer.clear();
  }
  aReadInfos.Clear();

  return rv;
}

NS_IMETHODIMP_(MozExternalRefCountType)
StackBasedEventTarget::AddRef()
{
  NS_NOTREACHED("Don't call me!");
  return 2;
}

NS_IMETHODIMP_(MozExternalRefCountType)
StackBasedEventTarget::Release()
{
  NS_NOTREACHED("Don't call me!");
  return 1;
}

NS_IMETHODIMP
StackBasedEventTarget::QueryInterface(REFNSIID aIID,
                                      void** aInstancePtr)
{
  NS_NOTREACHED("Don't call me!");
  return NS_NOINTERFACE;
}

NS_IMETHODIMP
ImmediateRunEventTarget::Dispatch(nsIRunnable* aRunnable,
                                  uint32_t aFlags)
{
  NS_ASSERTION(aRunnable, "Null pointer!");

  nsCOMPtr<nsIRunnable> runnable(aRunnable);
  DebugOnly<nsresult> rv =
    runnable->Run();
  MOZ_ASSERT(NS_SUCCEEDED(rv));
  return NS_OK;
}

NS_IMETHODIMP
ImmediateRunEventTarget::IsOnCurrentThread(bool* aIsOnCurrentThread)
{
  *aIsOnCurrentThread = true;
  return NS_OK;
}

NS_IMETHODIMP
TransactionPoolEventTarget::Dispatch(nsIRunnable* aRunnable,
                                     uint32_t aFlags)
{
  MOZ_CRASH("Remove me!");

  return NS_OK;
}

NS_IMETHODIMP
TransactionPoolEventTarget::IsOnCurrentThread(bool* aIsOnCurrentThread)
{
  *aIsOnCurrentThread = false;
  return NS_OK;
}

NS_IMETHODIMP
NoDispatchEventTarget::Dispatch(nsIRunnable* aRunnable,
                                uint32_t aFlags)
{
  nsCOMPtr<nsIRunnable> runnable = aRunnable;
  return NS_OK;
}

NS_IMETHODIMP
NoDispatchEventTarget::IsOnCurrentThread(bool* aIsOnCurrentThread)
{
  *aIsOnCurrentThread = true;
  return NS_OK;
}
