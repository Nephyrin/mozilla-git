/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#if !defined(WebMReader_h_)
#define WebMReader_h_

#include <stdint.h>

#include "nsDeque.h"
#include "MediaDecoderReader.h"
#include "nsAutoRef.h"
#include "nestegg/nestegg.h"

#define VPX_DONT_DEFINE_STDINT_TYPES
#include "vpx/vpx_codec.h"

#ifdef MOZ_TREMOR
#include "tremor/ivorbiscodec.h"
#else
#include "vorbis/codec.h"
#endif

#ifdef MOZ_OPUS
#include "OpusParser.h"
#endif

namespace mozilla {

class WebMBufferedState;

// Holds a nestegg_packet, and its file offset. This is needed so we
// know the offset in the file we've played up to, in order to calculate
// whether it's likely we can play through to the end without needing
// to stop to buffer, given the current download rate.
class NesteggPacketHolder {
public:
  NesteggPacketHolder(nestegg_packet* aPacket, int64_t aOffset)
    : mPacket(aPacket), mOffset(aOffset)
  {
    MOZ_COUNT_CTOR(NesteggPacketHolder);
  }
  ~NesteggPacketHolder() {
    MOZ_COUNT_DTOR(NesteggPacketHolder);
    nestegg_free_packet(mPacket);
  }
  nestegg_packet* mPacket;
  // Offset in bytes. This is the offset of the end of the Block
  // which contains the packet.
  int64_t mOffset;
private:
  // Copy constructor and assignment operator not implemented. Don't use them!
  NesteggPacketHolder(const NesteggPacketHolder &aOther);
  NesteggPacketHolder& operator= (NesteggPacketHolder const& aOther);
};

// Thread and type safe wrapper around nsDeque.
class PacketQueueDeallocator : public nsDequeFunctor {
  virtual void* operator() (void* aObject) {
    delete static_cast<NesteggPacketHolder*>(aObject);
    return nullptr;
  }
};

// Typesafe queue for holding nestegg packets. It has
// ownership of the items in the queue and will free them
// when destroyed.
class WebMPacketQueue : private nsDeque {
 public:
   WebMPacketQueue()
     : nsDeque(new PacketQueueDeallocator())
   {}

  ~WebMPacketQueue() {
    Reset();
  }

  inline int32_t GetSize() {
    return nsDeque::GetSize();
  }

  inline void Push(NesteggPacketHolder* aItem) {
    NS_ASSERTION(aItem, "NULL pushed to WebMPacketQueue");
    nsDeque::Push(aItem);
  }

  inline void PushFront(NesteggPacketHolder* aItem) {
    NS_ASSERTION(aItem, "NULL pushed to WebMPacketQueue");
    nsDeque::PushFront(aItem);
  }

  inline NesteggPacketHolder* PopFront() {
    return static_cast<NesteggPacketHolder*>(nsDeque::PopFront());
  }

  void Reset() {
    while (GetSize() > 0) {
      delete PopFront();
    }
  }
};

class WebMReader : public MediaDecoderReader
{
public:
  explicit WebMReader(AbstractMediaDecoder* aDecoder);

protected:
  ~WebMReader();

public:
  virtual nsresult Init(MediaDecoderReader* aCloneDonor);
  virtual nsresult ResetDecode();
  virtual bool DecodeAudioData();

  // If the Theora granulepos has not been captured, it may read several packets
  // until one with a granulepos has been captured, to ensure that all packets
  // read have valid time info.
  virtual bool DecodeVideoFrame(bool &aKeyframeSkip,
                                  int64_t aTimeThreshold);

  virtual bool HasAudio()
  {
    NS_ASSERTION(mDecoder->OnDecodeThread(), "Should be on decode thread.");
    return mHasAudio;
  }

  virtual bool HasVideo()
  {
    NS_ASSERTION(mDecoder->OnDecodeThread(), "Should be on decode thread.");
    return mHasVideo;
  }

  virtual nsresult ReadMetadata(MediaInfo* aInfo,
                                MetadataTags** aTags);
  virtual void Seek(int64_t aTime, int64_t aStartTime, int64_t aEndTime,
                    int64_t aCurrentTime);
  virtual nsresult GetBuffered(dom::TimeRanges* aBuffered);
  virtual void NotifyDataArrived(const char* aBuffer, uint32_t aLength,
                                 int64_t aOffset);
  virtual int64_t GetEvictionOffset(double aTime);

  virtual bool IsMediaSeekable() MOZ_OVERRIDE;

protected:
  // Value passed to NextPacket to determine if we are reading a video or an
  // audio packet.
  enum TrackType {
    VIDEO = 0,
    AUDIO = 1
  };

  // Read a packet from the nestegg file. Returns nullptr if all packets for
  // the particular track have been read. Pass VIDEO or AUDIO to indicate the
  // type of the packet we want to read.
  nsReturnRef<NesteggPacketHolder> NextPacket(TrackType aTrackType);

  // Pushes a packet to the front of the video packet queue.
  virtual void PushVideoPacket(NesteggPacketHolder* aItem);

#ifdef MOZ_OPUS
  // Setup opus decoder
  bool InitOpusDecoder();
#endif

  // Decode a nestegg packet of audio data. Push the audio data on the
  // audio queue. Returns true when there's more audio to decode,
  // false if the audio is finished, end of file has been reached,
  // or an un-recoverable read error has occured. The reader's monitor
  // must be held during this call. The caller is responsible for freeing
  // aPacket.
  bool DecodeAudioPacket(nestegg_packet* aPacket, int64_t aOffset);
  bool DecodeVorbis(const unsigned char* aData, size_t aLength,
                    int64_t aOffset, uint64_t aTstampUsecs,
                    int32_t* aTotalFrames);
#ifdef MOZ_OPUS
  bool DecodeOpus(const unsigned char* aData, size_t aLength,
                  int64_t aOffset, uint64_t aTstampUsecs,
                  nestegg_packet* aPacket);
#endif

  // Release context and set to null. Called when an error occurs during
  // reading metadata or destruction of the reader itself.
  void Cleanup();

  virtual nsresult SeekInternal(int64_t aTime, int64_t aStartTime);

private:
  // libnestegg context for webm container. Access on state machine thread
  // or decoder thread only.
  nestegg* mContext;

  // VP8 decoder state
  vpx_codec_ctx_t mVPX;

  // Vorbis decoder state
  vorbis_info mVorbisInfo;
  vorbis_comment mVorbisComment;
  vorbis_dsp_state mVorbisDsp;
  vorbis_block mVorbisBlock;
  int64_t mPacketCount;

#ifdef MOZ_OPUS
  // Opus decoder state
  nsAutoPtr<OpusParser> mOpusParser;
  OpusMSDecoder *mOpusDecoder;
  uint16_t mSkip;        // Samples left to trim before playback.
  uint64_t mSeekPreroll; // Nanoseconds to discard after seeking.
#endif

  // Queue of video and audio packets that have been read but not decoded. These
  // must only be accessed from the state machine thread.
  WebMPacketQueue mVideoPackets;
  WebMPacketQueue mAudioPackets;

  // Index of video and audio track to play
  uint32_t mVideoTrack;
  uint32_t mAudioTrack;

  // Time in microseconds of the start of the first audio frame we've decoded.
  int64_t mAudioStartUsec;

  // Number of audio frames we've decoded since decoding began at mAudioStartMs.
  uint64_t mAudioFrames;

  // Number of microseconds that must be discarded from the start of the Stream.
  uint64_t mCodecDelay;

  // Calculate the frame duration from the last decodeable frame using the
  // previous frame's timestamp.  In NS.
  uint64_t mLastVideoFrameTime;

  // Parser state and computed offset-time mappings.  Shared by multiple
  // readers when decoder has been cloned.  Main thread only.
  nsRefPtr<WebMBufferedState> mBufferedState;

  // Size of the frame initially present in the stream. The picture region
  // is defined as a ratio relative to this.
  nsIntSize mInitialFrame;

  // Picture region, as relative to the initial frame size.
  nsIntRect mPicture;

  // Codec ID of audio track
  int mAudioCodec;
  // Codec ID of video track
  int mVideoCodec;

  // Booleans to indicate if we have audio and/or video data
  bool mHasVideo;
  bool mHasAudio;

#ifdef MOZ_OPUS
  // Opus padding should only be discarded on the final packet.  Once this
  // is set to true, if the reader attempts to decode any further packets it
  // will raise an error so we can indicate that the file is invalid.
  bool mPaddingDiscarded;
#endif
};

} // namespace mozilla

#endif
