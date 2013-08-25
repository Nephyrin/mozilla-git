/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Combines a lot of the Mozilla networking interfaces into a sane interface for
 * simple(r) use of sockets code.
 *
 * This implements nsIServerSocketListener, nsIStreamListener,
 * nsIRequestObserver, nsITransportEventSink, nsIBadCertListener2,
 * nsISSLErrorListener, and nsIProtocolProxyCallback.
 *
 * This uses nsISocketTransportServices, nsIServerSocket, nsIThreadManager,
 * nsIBinaryInputStream, nsIScriptableInputStream, nsIInputStreamPump,
 * nsIProxyService, nsIProxyInfo.
 *
 * High-level methods:
 *   connect(<host>, <port>[, ("starttls" | "ssl" | "udp") [, <proxy>]])
 *   disconnect()
 *   listen(<port>)
 *   stopListening()
 *   sendData(String <data>[, <logged data>])
 *   sendString(String <data>[, <encoding>[, <logged data>]])
 *   sendBinaryData(<arraybuffer data>)
 *   startTLS()
 *   resetPingTimer()
 *   cancelDisconnectTimer()
 *
 * High-level properties:
 *   binaryMode
 *   delimiter
 *   inputSegmentSize
 *   outputSegmentSize
 *   proxyFlags
 *   connectTimeout (default is no timeout)
 *   readWriteTimeout (default is no timeout)
 *   isConnected
 *   sslStatus
 *
 * Users should "subclass" this object, i.e. set their .__proto__ to be it. And
 * then implement:
 *   onConnection()
 *   onConnectionHeard()
 *   onConnectionTimedOut()
 *   onConnectionReset()
 *   onBadCertificate(boolean aIsSslError, AString aNSSErrorMessage)
 *   onConnectionClosed()
 *   onDataReceived(String <data>)
 *   <length handled> = onBinaryDataReceived(ArrayBuffer <data>)
 *   onTransportStatus(nsISocketTransport <transport>, nsresult <status>,
 *                     unsigned long <progress>, unsigned long <progress max>)
 *   sendPing()
 *   LOG(<message>)
 *   DEBUG(<message>)
 *
 * Optional features:
 *   The ping functionality: Included in the socket object is a higher level
 *   "ping" messaging system, which is commonly used in instant messaging
 *   protocols. The ping functionality works by calling a user defined method,
 *   sendPing(), if resetPingTimer() is not called after two minutes. If no
 *   ping response is received after 30 seconds, the socket will disconnect.
 *   Thus, a socket using this functionality should:
 *     1. Implement sendPing() to send an appropriate ping message for the
 *        protocol.
 *     2. Call resetPingTimer() to start the ping messages.
 *     3. Call resetPingTimer() each time a message is received (i.e. the
 *        socket is known to still be alive).
 *     4. Call cancelDisconnectTimer() when a ping response is received.
 */

/*
 * To Do:
 *   Add a message queue to keep from flooding a server (just an array, just
 *     keep shifting the first element off and calling as setTimeout for the
 *     desired flood time?).
 */

const EXPORTED_SYMBOLS = ["Socket"];

const {classes: Cc, interfaces: Ci, results: Cr, utils: Cu} = Components;
Cu.import("resource://gre/modules/Services.jsm");
Cu.import("resource:///modules/ArrayBufferUtils.jsm");
Cu.import("resource:///modules/imXPCOMUtils.jsm");

// Network errors see: netwerk/base/public/nsNetError.h
const NS_ERROR_MODULE_NETWORK = 2152398848;
const NS_ERROR_CONNECTION_REFUSED = NS_ERROR_MODULE_NETWORK + 13;
const NS_ERROR_NET_TIMEOUT = NS_ERROR_MODULE_NETWORK + 14;
const NS_ERROR_NET_RESET = NS_ERROR_MODULE_NETWORK + 20;
const NS_ERROR_UNKNOWN_HOST = NS_ERROR_MODULE_NETWORK + 30;
const NS_ERROR_UNKNOWN_PROXY_HOST = NS_ERROR_MODULE_NETWORK + 42;
const NS_ERROR_PROXY_CONNECTION_REFUSED = NS_ERROR_MODULE_NETWORK + 72;

const BinaryInputStream =
  Components.Constructor("@mozilla.org/binaryinputstream;1",
                         "nsIBinaryInputStream", "setInputStream");
const BinaryOutputStream =
  Components.Constructor("@mozilla.org/binaryoutputstream;1",
                         "nsIBinaryOutputStream", "setOutputStream");
const ScriptableInputStream =
  Components.Constructor("@mozilla.org/scriptableinputstream;1",
                         "nsIScriptableInputStream", "init");
const ServerSocket =
  Components.Constructor("@mozilla.org/network/server-socket;1",
                         "nsIServerSocket", "init");
const InputStreamPump =
  Components.Constructor("@mozilla.org/network/input-stream-pump;1",
                         "nsIInputStreamPump", "init");
const ScriptableUnicodeConverter =
  Components.Constructor("@mozilla.org/intl/scriptableunicodeconverter",
                         "nsIScriptableUnicodeConverter");

const Socket = {
  // Use this to use binary mode for the
  binaryMode: false,

  // Set this for non-binary mode to automatically parse the stream into chunks
  // separated by delimiter.
  delimiter: "",

  // Set this for the segment size of outgoing binary streams.
  outputSegmentSize: 0,

  // Flags used by nsIProxyService when resolving a proxy.
  proxyFlags: Ci.nsIProtocolProxyService.RESOLVE_PREFER_SOCKS_PROXY,

  // Time (in seconds) for nsISocketTransport to continue trying before
  // reporting a failure, 0 is forever.
  connectTimeout: 0,
  readWriteTimeout: 0,

  // A nsISSLStatus instance giving details about the certificate error.
  sslStatus: null,

  /*
   *****************************************************************************
   ******************************* Public methods ******************************
   *****************************************************************************
   */
  // Synchronously open a connection.
  connect: function(aHost, aPort, aSecurity, aProxy) {
    if (Services.io.offline)
      throw Cr.NS_ERROR_FAILURE;

    this.LOG("Connecting to: " + aHost + ":" + aPort);
    this.host = aHost;
    this.port = aPort;

    // Array of security options
    this.security = aSecurity || [];

    // Choose a proxy, use the given one, otherwise get one from the proxy
    // service
    if (aProxy)
      this._createTransport(aProxy);
    else {
      try {
        // Attempt to get a default proxy from the proxy service.
        let proxyService = Cc["@mozilla.org/network/protocol-proxy-service;1"]
                              .getService(Ci.nsIProtocolProxyService);

        // Add a URI scheme since, by default, some protocols (i.e. IRC) don't
        // have a URI scheme before the host.
        let uri = Services.io.newURI("http://" + this.host, null, null);
        // This will return null when the result is known immediately and
        // the callback will just be dispatched to the current thread.
        this._proxyCancel = proxyService.asyncResolve(uri, this.proxyFlags, this);
      } catch(e) {
        Cu.reportError(e);
        // We had some error getting the proxy service, just don't use one.
        this._createTransport(null);
      }
    }
  },

  // Disconnect all open streams.
  disconnect: function() {
    this.LOG("Disconnect");

    // Close all input and output streams.
    if ("_inputStream" in this) {
      this._inputStream.close();
      delete this._inputStream;
    }
    if ("_outputStream" in this) {
      this._outputStream.close();
      delete this._outputStream;
    }
    if ("transport" in this) {
      this.transport.close(Cr.NS_OK);
      delete this.transport;
    }

    if ("_proxyCancel" in this) {
      if (this._proxyCancel)
        this._proxyCancel.cancel(Cr.NS_ERROR_ABORT); // Has to give a failure code
      delete this._proxyCancel;
    }

    if (this._pingTimer) {
      clearTimeout(this._pingTimer);
      delete this._pingTimer;
    }
    this.cancelDisconnectTimer();
  },

  // Listen for a connection on a port.
  // XXX take a timeout and then call stopListening
  listen: function(port) {
    this.LOG("Listening on port " + port);

    this.serverSocket = new ServerSocket(port, false, -1);
    this.serverSocket.asyncListen(this);
  },

  // Stop listening for a connection.
  stopListening: function() {
    this.LOG("Stop listening");
    // Close the socket to stop listening.
    if ("serverSocket" in this)
      this.serverSocket.close();
  },

  // Send data on the output stream. Provide aLoggedData to log something
  // different than what is actually sent.
  sendData: function(/* string */ aData, aLoggedData) {
    this.LOG("Sending:\n" + (aLoggedData || aData));

    try {
      this._outputStream.write(aData, aData.length);
    } catch(e) {
      Cu.reportError(e);
    }
  },

  // Send a string to the output stream after converting the encoding. Provide
  // aLoggedData to log something different than what is actually sent.
  sendString: function(aString, aEncoding, aLoggedData) {
    this.LOG("Sending:\n" + (aLoggedData || aString));

    let converter = new ScriptableUnicodeConverter();
    converter.charset = aEncoding || "UTF-8";
    try {
      let stream = converter.convertToInputStream(aString);
      this._outputStream.writeFrom(stream, stream.available());
    } catch(e) {
      Cu.reportError(e);
    }
  },

  sendBinaryData: function(/* ArrayBuffer */ aData) {
    this.LOG("Sending binary data: <" + ArrayBufferToHexString(aData) + ">");

    let byteArray = ArrayBufferToBytes(aData);
    try {
      // Send the data as a byte array
      this._binaryOutputStream.writeByteArray(byteArray, byteArray.length);
    } catch(e) {
      Cu.reportError(e);
    }
  },

  isConnected: false,

  startTLS: function() {
    this.transport.securityInfo.QueryInterface(Ci.nsISSLSocketControl).StartTLS();
  },

  // If using the ping functionality, this should be called whenever a message is
  // received (e.g. when it is known the socket is still open). Calling this for
  // the first time enables the ping functionality.
  resetPingTimer: function() {
    if (this._pingTimer)
      clearTimeout(this._pingTimer);
    // Send a ping every 2 minutes if there's no traffic on the socket.
    this._pingTimer = setTimeout(this._sendPing.bind(this), 120000);
  },

  // If using the ping functionality, this should be called when a ping receives
  // a response.
  cancelDisconnectTimer: function() {
    if (!this._disconnectTimer)
      return;
    clearTimeout(this._disconnectTimer);
    delete this._disconnectTimer;
  },

  /*
   *****************************************************************************
   ***************************** Interface methods *****************************
   *****************************************************************************
   */
  /*
   * nsIProtocolProxyCallback methods
   */
  onProxyAvailable: function(aRequest, aURI, aProxyInfo, aStatus) {
    if (!("_proxyCancel" in this)) {
      this.LOG("onProxyAvailable called, but disconnect() was called before.");
      return;
    }

    if (aProxyInfo) {
      if (aProxyInfo.type == "http") {
        this.LOG("ignoring http proxy");
        aProxyInfo = null;
      }
      else {
        this.LOG("using " + aProxyInfo.type + " proxy: " +
                 aProxyInfo.host + ":" + aProxyInfo.port);
      }
    }
    this._createTransport(aProxyInfo);
    delete this._proxyCancel;
  },

  /*
   * nsIServerSocketListener methods
   */
  // Called after a client connection is accepted when we're listening for one.
  onSocketAccepted: function(aServerSocket, aTransport) {
    this.LOG("onSocketAccepted");
    // Store the values
    this.transport = aTransport;
    this.host = this.transport.host;
    this.port = this.transport.port;

    this._resetBuffers();
    this._openStreams();

    this.isConnected = true;
    this.onConnectionHeard();
    this.stopListening();
  },
  // Called when the listening socket stops for some reason.
  // The server socket is effectively dead after this notification.
  onStopListening: function(aSocket, aStatus) {
    this.LOG("onStopListening");
    if ("serverSocket" in this)
      delete this.serverSocket;
  },

  /*
   * nsIStreamListener methods
   */
  // onDataAvailable, called by Mozilla's networking code.
  // Buffers the data, and parses it into discrete messages.
  onDataAvailable: function(aRequest, aContext, aInputStream, aOffset, aCount) {
    if (this.binaryMode) {
      // Load the data from the stream
      this._incomingDataBuffer = this._incomingDataBuffer
                                     .concat(this._binaryInputStream
                                                 .readByteArray(aCount));

      let size = this._incomingDataBuffer.length;

      // Create a new ArrayBuffer.
      let buffer = new ArrayBuffer(size);
      let uintArray = new Uint8Array(buffer);

      // Set the data into the array while saving the extra data.
      uintArray.set(this._incomingDataBuffer);

      // Notify we've received data.
      // Variable data size, the callee must return how much data was handled.
      size = this.onBinaryDataReceived(buffer);

      // Remove the handled data.
      this._incomingDataBuffer.splice(0, size);
    } else {
      if (this.delimiter) {
        // Load the data from the stream
        this._incomingDataBuffer += this._scriptableInputStream.read(aCount);
        let data = this._incomingDataBuffer.split(this.delimiter);

        // Store the (possibly) incomplete part
        this._incomingDataBuffer = data.pop();

        // Send each string to the handle data function
        data.forEach(this.onDataReceived, this);
      } else {
        // Send the whole string to the handle data function
        this.onDataReceived(this._scriptableInputStream.read(aCount));
      }
    }
  },

  /*
   * nsIRequestObserver methods
   */
  // Signifies the beginning of an async request
  onStartRequest: function(aRequest, aContext) {
    this.DEBUG("onStartRequest");
  },
  // Called to signify the end of an asynchronous request.
  onStopRequest: function(aRequest, aContext, aStatus) {
    this.DEBUG("onStopRequest (" + aStatus + ")");
    delete this.isConnected;
    if (aStatus == NS_ERROR_NET_RESET)
      this.onConnectionReset();
    else if (aStatus == NS_ERROR_NET_TIMEOUT)
      this.onConnectionTimedOut();
    else if (!Components.isSuccessCode(aStatus)) {
      let nssErrorsService =
        Cc["@mozilla.org/nss_errors_service;1"].getService(Ci.nsINSSErrorsService);
      if ((aStatus <= nssErrorsService.getXPCOMFromNSSError(nssErrorsService.NSS_SEC_ERROR_BASE) &&
           aStatus >= nssErrorsService.getXPCOMFromNSSError(nssErrorsService.NSS_SEC_ERROR_LIMIT - 1)) ||
          (aStatus <= nssErrorsService.getXPCOMFromNSSError(nssErrorsService.NSS_SSL_ERROR_BASE) &&
           aStatus >= nssErrorsService.getXPCOMFromNSSError(nssErrorsService.NSS_SSL_ERROR_LIMIT - 1))) {
        this.onBadCertificate(nssErrorsService.getErrorClass(aStatus) ==
                              nssErrorsService.ERROR_CLASS_SSL_PROTOCOL,
                              nssErrorsService.getErrorMessage(aStatus));
        return;
      }
    }
    this.onConnectionClosed();
  },

  /*
   * nsIBadCertListener2
   */
  // Called when there's an error, return true to suppress the modal alert.
  // Whatever this function returns, NSS will close the connection.
  notifyCertProblem: function(aSocketInfo, aStatus, aTargetSite) {
    this.sslStatus = aStatus;
    return true;
  },

  /*
   * nsISSLErrorListener
   */
  notifySSLError: function(aSocketInfo, aError, aTargetSite) {
    this.sslStatus = null;
    return true;
  },

  /*
   * nsITransportEventSink methods
   */
  onTransportStatus: function(aTransport, aStatus, aProgress, aProgressmax) {
    // Don't send status change notifications after the socket has been closed.
    // The event sink can't be removed after opening the transport, so we can't
    // do better than adding a null check here.
    if (!this.transport)
      return;

    const nsITransportEventSinkStatus = {
         0x804b0003: "STATUS_RESOLVING",
         0x804b000b: "STATUS_RESOLVED",
         0x804b0007: "STATUS_CONNECTING_TO",
         0x804b0004: "STATUS_CONNECTED_TO",
         0x804b0005: "STATUS_SENDING_TO",
         0x804b000a: "STATUS_WAITING_FOR",
         0x804b0006: "STATUS_RECEIVING_FROM"
    };
    let status = nsITransportEventSinkStatus[aStatus];
    this.DEBUG("onTransportStatus(" + (status || ("0x" + aStatus.toString(16))) +")");

    if (status == "STATUS_CONNECTED_TO") {
      this.isConnected = true;
      // Notify that the connection has been established.
      this.onConnection();
    }
  },

  /*
   *****************************************************************************
   ****************************** Private methods ******************************
   *****************************************************************************
   */
  _resetBuffers: function() {
    this._incomingDataBuffer = this.binaryMode ? [] : "";
    this._outgoingDataBuffer = [];
  },

  _createTransport: function(aProxy) {
    this.proxy = aProxy;

    // Empty incoming and outgoing data storage buffers
    this._resetBuffers();

    // Create a socket transport
    let socketTS = Cc["@mozilla.org/network/socket-transport-service;1"]
                      .getService(Ci.nsISocketTransportService);
    this.transport = socketTS.createTransport(this.security,
                                              this.security.length, this.host,
                                              this.port, this.proxy);

    this._openStreams();
  },

  // Open the incoming and outgoing streams, and init the nsISocketTransport.
  _openStreams: function() {
    // Security notification callbacks (must support nsIBadCertListener2 and
    // nsISSLErrorListener for SSL connections, and possibly other interfaces).
    this.transport.securityCallbacks = this;

    // Set the timeouts for the nsISocketTransport for both a connect event and
    // a read/write. Only set them if the user has provided them.
    if (this.connectTimeout) {
      this.transport.setTimeout(Ci.nsISocketTransport.TIMEOUT_CONNECT,
                                this.connectTimeout);
    }
    if (this.readWriteTimeout) {
      this.transport.setTimeout(Ci.nsISocketTransport.TIMEOUT_READ_WRITE,
                                this.readWriteTimeout);
    }

    this.transport.setEventSink(this, Services.tm.currentThread);

    // No limit on the output stream buffer
    this._outputStream =
      this.transport.openOutputStream(0, this.outputSegmentSize, -1);
    if (!this._outputStream)
      throw "Error getting output stream.";

    this._inputStream = this.transport.openInputStream(0, // flags
                                                       0, // Use default segment size
                                                       0); // Use default segment count
    if (!this._inputStream)
      throw "Error getting input stream.";

    if (this.binaryMode) {
      // Handle binary mode
      this._binaryInputStream = new BinaryInputStream(this._inputStream);
      this._binaryOutputStream = new BinaryOutputStream(this._outputStream);
    } else {
      // Handle character mode
      this._scriptableInputStream =
        new ScriptableInputStream(this._inputStream);
    }

    this.pump = new InputStreamPump(this._inputStream, // Data to read
                                    -1, // Current offset
                                    -1, // Read all data
                                    0, // Use default segment size
                                    0, // Use default segment length
                                    false); // Do not close when done
    this.pump.asyncRead(this, this);
  },

  _pingTimer: null,
  _disconnectTimer: null,
  _sendPing: function() {
    delete this._pingTimer;
    this.sendPing();
    this._disconnectTimer = setTimeout(this.onConnectionTimedOut.bind(this),
                                       30000);
  },

  /*
   *****************************************************************************
   ********************* Methods for subtypes to override **********************
   *****************************************************************************
   */
  LOG: function(aString) { },
  DEBUG: function(aString) { },
  // Called when a connection is established.
  onConnection: function() { },
  // Called when a socket is accepted after listening.
  onConnectionHeard: function() { },
  // Called when a connection times out.
  onConnectionTimedOut: function() { },
  // Called when a socket request's network is reset.
  onConnectionReset: function() { },
  // Called when the certificate provided by the server didn't satisfy NSS.
  onBadCertificate: function(aNSSErrorMessage) { },
  // Called when the other end has closed the connection.
  onConnectionClosed: function() { },

  // Called when ASCII data is available.
  onDataReceived: function(/* string */ aData) { },

  // Called when binary data is available.
  onBinaryDataReceived: function(/* ArrayBuffer */ aData) { },

  // If using the ping functionality, this is called when a new ping message
  // should be sent on the socket.
  sendPing: function() { },

  /* QueryInterface and nsIInterfaceRequestor implementations */
  _interfaces: [Ci.nsIServerSocketListener, Ci.nsIStreamListener,
                Ci.nsIRequestObserver, Ci.nsITransportEventSink,
                Ci.nsIBadCertListener2, Ci.nsISSLErrorListener,
                Ci.nsIProtocolProxyCallback],
  QueryInterface: function(iid) {
    if (iid.equals(Ci.nsISupports) ||
        this._interfaces.some(function(i) i.equals(iid)))
      return this;

    throw Cr.NS_ERROR_NO_INTERFACE;
  },
  getInterface: function(iid) this.QueryInterface(iid)
};
