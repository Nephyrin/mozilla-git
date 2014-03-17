/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * Native (xpcom) implementation of key OS.File functions
 */

"use strict";

this.EXPORTED_SYMBOLS = ["read"];

let {results: Cr, utils: Cu, interfaces: Ci} = Components;

let SharedAll = Cu.import("resource://gre/modules/osfile/osfile_shared_allthreads.jsm", {});

let SysAll = {};
if (SharedAll.Constants.Win) {
  Cu.import("resource://gre/modules/osfile/osfile_win_allthreads.jsm", SysAll);
} else if (SharedAll.Constants.libc) {
  Cu.import("resource://gre/modules/osfile/osfile_unix_allthreads.jsm", SysAll);
} else {
  throw new Error("I am neither under Windows nor under a Posix system");
}
let {Promise} = Cu.import("resource://gre/modules/Promise.jsm", {});
let {XPCOMUtils} = Cu.import("resource://gre/modules/XPCOMUtils.jsm", {});

/**
 * The native service holding the implementation of the functions.
 */
XPCOMUtils.defineLazyServiceGetter(this,
  "Internals",
  "@mozilla.org/toolkit/osfile/native-internals;1",
  "nsINativeOSFileInternalsService");

/**
 * Native implementation of OS.File.read
 *
 * This implementation does not handle option |compression|.
 */
this.read = function(path, options = {}) {
  // Sanity check on types of options
  if ("encoding" in options && typeof options.encoding != "string") {
    return Promise.reject(new TypeError("Invalid type for option encoding"));
  }
  if ("compression" in options && typeof options.compression != "string") {
    return Promise.reject(new TypeError("Invalid type for option compression"));
  }
  if ("bytes" in options && typeof options.bytes != "number") {
    return Promise.reject(new TypeError("Invalid type for option bytes"));
  }

  let deferred = Promise.defer();
  Internals.read(path,
    options,
    function onSuccess(success) {
      success.QueryInterface(Ci.nsINativeOSFileResult);
      if ("outExecutionDuration" in options) {
        options.outExecutionDuration =
          success.executionDurationMS +
          (options.outExecutionDuration || 0);
      }
      deferred.resolve(success.result);
    },
    function onError(operation, oserror) {
      deferred.reject(new SysAll.Error(operation, oserror, path));
    }
  );
  return deferred.promise;
};
