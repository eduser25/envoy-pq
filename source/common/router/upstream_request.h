#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "envoy/http/codec.h"
#include "envoy/http/codes.h"
#include "envoy/http/conn_pool.h"
#include "envoy/http/filter.h"
#include "envoy/stats/scope.h"
#include "envoy/tcp/conn_pool.h"

#include "source/common/buffer/watermark_buffer.h"
#include "source/common/common/cleanup.h"
#include "source/common/common/hash.h"
#include "source/common/common/hex.h"
#include "source/common/common/linked_object.h"
#include "source/common/common/logger.h"
#include "source/common/config/well_known_names.h"
#include "source/common/runtime/runtime_features.h"
#include "source/common/stream_info/stream_info_impl.h"

namespace Envoy {
namespace Router {

class GenericUpstream;
class GenericConnectionPoolCallbacks;
class RouterFilterInterface;
class UpstreamRequest;

// The base request for Upstream.
class UpstreamRequest : public Logger::Loggable<Logger::Id::router>,
                        public UpstreamToDownstream,
                        public LinkedObject<UpstreamRequest>,
                        public GenericConnectionPoolCallbacks,
                        public Event::DeferredDeletable {
public:
  UpstreamRequest(RouterFilterInterface& parent, std::unique_ptr<GenericConnPool>&& conn_pool,
                  bool can_send_early_data, bool can_use_http3);
  ~UpstreamRequest() override;
  void deleteIsPending() override { cleanUp(); }

  // To be called from the destructor, or prior to deferred delete.
  void cleanUp();

  void acceptHeadersFromRouter(bool end_stream);
  void acceptDataFromRouter(Buffer::Instance& data, bool end_stream);
  void acceptTrailersFromRouter(const Http::RequestTrailerMap& trailers);
  void acceptMetadataFromRouter(Http::MetadataMapPtr&& metadata_map_ptr);

  void resetStream();
  void setupPerTryTimeout();
  void maybeEndDecode(bool end_stream);
  void onUpstreamHostSelected(Upstream::HostDescriptionConstSharedPtr host);

  // Http::StreamDecoder
  void decodeData(Buffer::Instance& data, bool end_stream) override;
  void decodeMetadata(Http::MetadataMapPtr&& metadata_map) override;

  // UpstreamToDownstream (Http::ResponseDecoder)
  void decode1xxHeaders(Http::ResponseHeaderMapPtr&& headers) override;
  void decodeHeaders(Http::ResponseHeaderMapPtr&& headers, bool end_stream) override;
  void decodeTrailers(Http::ResponseTrailerMapPtr&& trailers) override;
  void dumpState(std::ostream& os, int indent_level) const override;

  // UpstreamToDownstream (Http::StreamCallbacks)
  void onResetStream(Http::StreamResetReason reason,
                     absl::string_view transport_failure_reason) override;
  void onAboveWriteBufferHighWatermark() override { disableDataFromDownstreamForFlowControl(); }
  void onBelowWriteBufferLowWatermark() override { enableDataFromDownstreamForFlowControl(); }
  // UpstreamToDownstream
  const Route& route() const override;
  OptRef<const Network::Connection> connection() const override;
  const Http::ConnectionPool::Instance::StreamOptions& upstreamStreamOptions() const override {
    return stream_options_;
  }

  void disableDataFromDownstreamForFlowControl();
  void enableDataFromDownstreamForFlowControl();

  // GenericConnPool
  void onPoolFailure(ConnectionPool::PoolFailureReason reason,
                     absl::string_view transport_failure_reason,
                     Upstream::HostDescriptionConstSharedPtr host) override;
  void onPoolReady(std::unique_ptr<GenericUpstream>&& upstream,
                   Upstream::HostDescriptionConstSharedPtr host,
                   const Network::Address::InstanceConstSharedPtr& upstream_local_address,
                   StreamInfo::StreamInfo& info, absl::optional<Http::Protocol> protocol) override;
  UpstreamToDownstream& upstreamToDownstream() override { return *this; }

  void clearRequestEncoder();
  void onStreamMaxDurationReached();

  struct DownstreamWatermarkManager : public Http::DownstreamWatermarkCallbacks {
    DownstreamWatermarkManager(UpstreamRequest& parent) : parent_(parent) {}

    // Http::DownstreamWatermarkCallbacks
    void onBelowWriteBufferLowWatermark() override;
    void onAboveWriteBufferHighWatermark() override;

    UpstreamRequest& parent_;
  };

  void readEnable();
  void encodeBodyAndTrailers();

  // Getters and setters
  Upstream::HostDescriptionConstSharedPtr& upstreamHost() { return upstream_host_; }
  void outlierDetectionTimeoutRecorded(bool recorded) {
    outlier_detection_timeout_recorded_ = recorded;
  }
  bool outlierDetectionTimeoutRecorded() { return outlier_detection_timeout_recorded_; }
  void retried(bool value) { retried_ = value; }
  bool retried() { return retried_; }
  bool grpcRqSuccessDeferred() { return grpc_rq_success_deferred_; }
  void grpcRqSuccessDeferred(bool deferred) { grpc_rq_success_deferred_ = deferred; }
  void upstreamCanary(bool value) { upstream_canary_ = value; }
  bool upstreamCanary() { return upstream_canary_; }
  bool awaitingHeaders() { return awaiting_headers_; }
  void recordTimeoutBudget(bool value) { record_timeout_budget_ = value; }
  bool createPerTryTimeoutOnRequestComplete() {
    return create_per_try_timeout_on_request_complete_;
  }
  bool encodeComplete() const { return router_sent_end_stream_; }
  // Exposes streamInfo for the upstream stream.
  StreamInfo::StreamInfo& streamInfo() { return stream_info_; }
  bool hadUpstream() const { return had_upstream_; }

private:
  StreamInfo::UpstreamTiming& upstreamTiming() {
    return stream_info_.upstreamInfo()->upstreamTiming();
  }
  bool shouldSendEndStream() {
    // Only encode end stream if the full request has been received, the body
    // has been sent, and any trailers or metadata have also been sent.
    return router_sent_end_stream_ && !buffered_request_body_ && !encode_trailers_ &&
           downstream_metadata_map_vector_.empty();
  }
  void addResponseHeadersSize(uint64_t size) {
    response_headers_size_ = response_headers_size_.value_or(0) + size;
  }
  void resetPerTryIdleTimer();
  void onPerTryTimeout();
  void onPerTryIdleTimeout();

  RouterFilterInterface& parent_;
  std::unique_ptr<GenericConnPool> conn_pool_;
  bool grpc_rq_success_deferred_;
  Event::TimerPtr per_try_timeout_;
  Event::TimerPtr per_try_idle_timeout_;
  std::unique_ptr<GenericUpstream> upstream_;
  absl::optional<Http::StreamResetReason> deferred_reset_reason_;
  Buffer::InstancePtr buffered_request_body_;
  Upstream::HostDescriptionConstSharedPtr upstream_host_;
  DownstreamWatermarkManager downstream_watermark_manager_{*this};
  Tracing::SpanPtr span_;
  StreamInfo::StreamInfoImpl stream_info_;
  const MonotonicTime start_time_;
  // This is wrapped in an optional, since we want to avoid computing zero size headers when in
  // reality we just didn't get a response back.
  absl::optional<uint64_t> response_headers_size_{};
  // Copies of upstream headers/trailers. These are only set if upstream
  // access logging is configured.
  Http::ResponseHeaderMapPtr upstream_headers_;
  Http::ResponseTrailerMapPtr upstream_trailers_;
  Http::MetadataMapVector downstream_metadata_map_vector_;

  // Tracks the number of times the flow of data from downstream has been disabled.
  uint32_t downstream_data_disabled_{};
  bool calling_encode_headers_ : 1;
  bool upstream_canary_ : 1;
  bool decode_complete_ : 1;
  bool router_sent_end_stream_ : 1;
  bool encode_trailers_ : 1;
  bool retried_ : 1;
  bool awaiting_headers_ : 1;
  bool outlier_detection_timeout_recorded_ : 1;
  // Tracks whether we deferred a per try timeout because the downstream request
  // had not been completed yet.
  bool create_per_try_timeout_on_request_complete_ : 1;
  // True if the CONNECT headers have been sent but proxying payload is paused
  // waiting for response headers.
  bool paused_for_connect_ : 1;

  // Sentinel to indicate if timeout budget tracking is configured for the cluster,
  // and if so, if the per-try histogram should record a value.
  bool record_timeout_budget_ : 1;
  // Track if one time clean up has been performed.
  bool cleaned_up_ : 1;
  bool had_upstream_ : 1;
  Http::ConnectionPool::Instance::StreamOptions stream_options_;
  Event::TimerPtr max_stream_duration_timer_;
};

} // namespace Router
} // namespace Envoy
