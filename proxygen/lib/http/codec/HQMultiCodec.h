/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <proxygen/lib/http/codec/HQControlCodec.h>
#include <proxygen/lib/http/codec/HQStreamCodec.h>
#include <proxygen/lib/http/codec/compress/QPACKCodec.h>

namespace proxygen { namespace hq {

class HQMultiCodec : public HQControlCodec {

 public:
  explicit HQMultiCodec(TransportDirection direction)
      : HQControlCodec(HTTPCodec::MaxStreamID,
                       direction,
                       StreamDirection::INGRESS, /* to match settings */
                       ingressSettings_,
                       UnidirectionalStreamType::CONTROL) {
    VLOG(4) << "creating " << getTransportDirectionString(direction)
            << " HQMultiCodec for stream " << streamId_;
    // Has to be explicitly enabled
    doubleGoaway_ = false;
    minUnseenStreamID_ = 0;
    minUnseenPushID_ = 0;
  }

  ~HQMultiCodec() override = default;

  void setControlStreamID(StreamID controlID) {
    streamId_ = controlID;
  }

  void setQPACKEncoderMaxDataFn(std::function<uint64_t()> qpackEncoderMaxData) {
    qpackEncoderMaxDataFn_ = std::move(qpackEncoderMaxData);
  }

  bool setCurrentStream(StreamID currentStream) {
    if (codecs_.find(currentStream) == codecs_.end()) {
      return false;
    }
    currentStream_ = currentStream;
    return true;
  }

  HTTPCodec& addCodec(StreamID streamId) {
    if (transportDirection_ == TransportDirection::DOWNSTREAM &&
        (streamId & 0x3) == 0 && streamId >= minUnseenStreamID_) {
      // only bump for client initiated bidi streams, for now
      minUnseenStreamID_ = streamId + 4;
    }
    auto res =
        codecs_.emplace(streamId,
                        std::make_unique<HQStreamCodec>(streamId,
                                                        transportDirection_,
                                                        qpackCodec_,
                                                        qpackEncoderWriteBuf_,
                                                        qpackDecoderWriteBuf_,
                                                        qpackEncoderMaxDataFn_,
                                                        settings_));
    auto& codec = res.first->second;
    codec->setCallback(callback_);
    return *codec;
  }

  void removeCodec(StreamID streamId) {
    codecs_.erase(streamId);
  }

  QPACKCodec& getQPACKCodec() {
    return qpackCodec_;
  }

  folly::IOBufQueue& getQPACKEncoderWriteBuf() {
    return qpackEncoderWriteBuf_;
  }

  folly::IOBufQueue& getQPACKDecoderWriteBuf() {
    return qpackDecoderWriteBuf_;
  }

  void setCallback(proxygen::HTTPCodec::Callback* callback) override {
    HQControlCodec::setCallback(callback);
    for (const auto& codec : codecs_) {
      codec.second->setCallback(callback);
    }
  }

  const std::string& getUserAgent() const override {
    // TODO
    static const std::string empty;
    return empty;
  }

  size_t onIngress(const folly::IOBuf& buf) override {
    auto res = getCurrentCodec().onIngress(buf);
    currentStream_ = HTTPCodec::MaxStreamID;
    return res;
  }

  void onIngressEOF() override {
    getCurrentCodec().onIngressEOF();
    currentStream_ = HTTPCodec::MaxStreamID;
  }

  bool isReusable() const override {
    return !sentGoaway_;
  }

  bool supportsParallelRequests() const override {
    return true;
  }

  size_t generateConnectionPreface(folly::IOBufQueue& /*writeBuf*/) override {
    return 0;
  }

  size_t generateSettingsAck(folly::IOBufQueue& /*writeBuf*/) override {
    return 0;
  }

  // It is possible to make HQStreamCodec egress stateless and avoid the
  // hashtable lookup in the generate* functions.
  void generateHeader(
      folly::IOBufQueue& writeBuf,
      StreamID stream,
      const HTTPMessage& msg,
      bool eom = false,
      HTTPHeaderSize* size = nullptr,
      const folly::Optional<HTTPHeaders>& extraHeaders = folly::none) override {
    getCodec(stream).generateHeader(
        writeBuf, stream, msg, eom, size, extraHeaders);
  }

  void generatePushPromise(folly::IOBufQueue& writeBuf,
                           StreamID stream,
                           const HTTPMessage& msg,
                           StreamID pushID,
                           bool eom = false,
                           HTTPHeaderSize* size = nullptr) override {
    getCodec(stream).generatePushPromise(
        writeBuf, stream, msg, pushID, eom, size);
  }

  size_t generateBody(folly::IOBufQueue& writeBuf,
                      StreamID stream,
                      std::unique_ptr<folly::IOBuf> chain,
                      folly::Optional<uint8_t> padding,
                      bool eom) override {
    return getCodec(stream).generateBody(
        writeBuf, stream, std::move(chain), padding, eom);
  }

  size_t generateTrailers(folly::IOBufQueue& writeBuf,
                          StreamID stream,
                          const HTTPHeaders& trailers) override {
    return getCodec(stream).generateTrailers(writeBuf, stream, trailers);
  }

  size_t generateEOM(folly::IOBufQueue& writeBuf, StreamID stream) override {
    return getCodec(stream).generateEOM(writeBuf, stream);
  }

  CompressionInfo getCompressionInfo() const override {
    return qpackCodec_.getCompressionInfo();
  }

  // HTTPCodec API
  uint32_t getDefaultWindowSize() const override {
    return std::numeric_limits<uint32_t>::max();
  }

  HTTPSettings* getEgressSettings() override {
    return &egressSettings_;
  }

  uint64_t nextPushID() {
    CHECK_EQ(transportDirection_, TransportDirection::DOWNSTREAM);
    return nextPushID_++;
  }

  void onIngressPushId(uint64_t pushId) {
    minUnseenPushID_ = std::max(minUnseenPushID_, pushId + 1);
  }

 protected:
  HTTPCodec& getCurrentCodec() {
    return getCodec(currentStream_);
  }

  HTTPCodec& getCodec(StreamID stream) {
    auto it = codecs_.find(stream);
    CHECK(it != codecs_.end()) << "stream=" << stream;
    return *it->second;
  }

  HTTPSettings ingressSettings_;
  HTTPSettings egressSettings_;
  StreamID currentStream_{HTTPCodec::MaxStreamID};
  folly::F14FastMap<StreamID, std::unique_ptr<HQStreamCodec>> codecs_;
  QPACKCodec qpackCodec_;
  folly::IOBufQueue qpackEncoderWriteBuf_{
      folly::IOBufQueue::cacheChainLength()};
  folly::IOBufQueue qpackDecoderWriteBuf_{
      folly::IOBufQueue::cacheChainLength()};
  std::function<uint64_t()> qpackEncoderMaxDataFn_;
  uint64_t nextPushID_{0};
};

}} // namespace proxygen::hq
