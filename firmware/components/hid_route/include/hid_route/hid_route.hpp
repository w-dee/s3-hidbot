#pragma once

#include <atomic>
#include <cstdint>
#include <limits>

namespace hid_route {

enum class OutputRoute : std::uint8_t {
    kNone,
    kUsb,
    // Public route v1 remains none|usb; route v2 adds BLE.
    kBle,
};

enum class Transition : std::uint8_t {
    kStable,
    kReleasing,
};

// Opaque route-authority epoch. Consumers may compare only for exact equality
// or inequality; this is not a route-change count or an ordering value.
using Generation = std::uint32_t;
using ConditionalInvalidationToken = std::uint32_t;
using UsbPublicationCut = std::uint32_t;
inline constexpr ConditionalInvalidationToken kNoConditionalInvalidationToken = 0;

struct Snapshot {
    OutputRoute desired = OutputRoute::kNone;
    OutputRoute active = OutputRoute::kNone;
    Generation generation = 0;
    Transition transition = Transition::kStable;
    bool invalidation_pending = false;
    // False means bounded observation could not prove a cross-field
    // publication. The returned values are still an internally coherent,
    // fail-closed none snapshot and must never be treated as ready.
    bool coherent = true;
};

enum class InvalidationClaimResult : std::uint8_t {
    kClaimedExact,
    kStale,
    kPending,
};

class StateMachine;

// A successful claim holds the route writer and the fail-closed invalidation
// gate until release(). It is stack-owned and cannot outlive StateMachine.
class ExactInvalidationClaim final {
  public:
    ExactInvalidationClaim() = default;
    ~ExactInvalidationClaim();
    ExactInvalidationClaim(const ExactInvalidationClaim &) = delete;
    ExactInvalidationClaim &operator=(const ExactInvalidationClaim &) = delete;

    bool retire();
    void release();
    bool active() const { return owner_ != nullptr; }

  private:
    friend class StateMachine;
    StateMachine *owner_ = nullptr;
    Snapshot expected_{};
    ConditionalInvalidationToken conditional_token_ =
        kNoConditionalInvalidationToken;
};

// This component owns output-route selection and its independent invalidation
// token only. Transport lifecycle, safety state, and report data remain in
// their respective transport/runtime components.
class StateMachine final {
  public:
    StateMachine();

    void initialize_cold_boot();
    // Bounded coherent observation. Readers never spin indefinitely: after a
    // small fixed retry budget, snapshot() returns a coherent fail-closed none
    // view with coherent=false. Accepted command responses still use the
    // immutable snapshot owned by their transition controller.
    Snapshot snapshot() const;
    bool matches(OutputRoute route, Generation generation) const;

    // The runtime route controller is the only production caller of this
    // transition. It is non-blocking: callback invalidation wins fail-closed.
    bool commit_usb_if_none();
    bool commit_usb_if_none(UsbPublicationCut expected_cut);
    // Route-v2 reaches this internal transition through the serialized
    // controller; route-v1 still rejects BLE before entering route state.
    bool commit_ble_if_none();

    // Serialized route-controller transitions publish
    // desired=none/active=<old>/releasing without retiring the old generation;
    // completion retires it only after the transport's safety boundary.
    bool begin_usb_release(Snapshot *stage_a);
    bool complete_usb_release_if_matches(Snapshot expected);
    bool begin_ble_release(Snapshot *stage_a);
    bool complete_ble_release_if_matches(Snapshot expected);

    // Invalidation is callback-safe and does not wait for a control executor.
    // It closes the unsafe gate before it attempts the committed transition.
    bool invalidate();
    bool invalidate_if_matches(Snapshot expected);
    UsbPublicationCut usb_publication_cut() const;
    void publish_usb_lifecycle_veto();

    // Exact callers may hold the writer/gate across an external-authority
    // recheck. A nonzero token identifies only this caller's cancelable
    // watchdog request; durable lifecycle invalidation is independent.
    InvalidationClaimResult claim_invalidation_if_matches(
        Snapshot expected, ConditionalInvalidationToken *conditional_token,
        ExactInvalidationClaim *claim);
    bool cancel_conditional_invalidation(
        Generation generation, ConditionalInvalidationToken conditional_token);

#ifdef HID_ROUTE_NATIVE_TEST
    using GenerationPublishedHook = void (*)(StateMachine &state);
    using WriterAcquiredHook = void (*)(StateMachine &state);
    void set_generation_for_test(Generation generation);
    void set_generation_published_hook_for_test(GenerationPublishedHook hook);
    void set_writer_acquired_hook_for_test(WriterAcquiredHook hook);
    void set_usb_publication_before_release_hook_for_test(
        WriterAcquiredHook hook);
    void set_usb_publication_after_release_hook_for_test(
        WriterAcquiredHook hook);
    void set_usb_publication_serial_for_test(UsbPublicationCut serial);
    void set_publication_busy_for_test(bool busy);
#endif

  private:
    friend class ExactInvalidationClaim;
    void begin_publication();
    void end_publication();
    bool try_enter_writer();
    void leave_writer();
    void leave_writer_with_handoff();
    bool invalidation_pending() const;
    bool invalidation_pending_without_usb_publication() const;
    static UsbPublicationCut advance_usb_publication_serial(
        UsbPublicationCut state);
    bool begin_usb_publication(UsbPublicationCut expected_cut,
                               UsbPublicationCut *identity);
    bool finish_usb_publication(UsbPublicationCut identity);
    void clear_usb_publication(UsbPublicationCut identity);
    bool publish_durable_invalidation(Generation generation);
    void process_durable_invalidation_locked();
    void clear_superseded_conditional_invalidation_locked();
    ConditionalInvalidationToken allocate_conditional_token();
    bool publish_conditional_invalidation(
        Generation generation, ConditionalInvalidationToken *conditional_token);
    bool conditional_invalidation_matches(
        Generation generation, ConditionalInvalidationToken conditional_token) const;
    bool commit_none_locked(OutputRoute expected_route, Generation expected_generation,
                            bool require_exact_match);
    bool commit_if_none(OutputRoute route, UsbPublicationCut expected_cut = 0);
    bool begin_release(OutputRoute route, Snapshot *stage_a);
    bool complete_release(OutputRoute route, Snapshot expected);
    bool retire_invalidation_claim(ExactInvalidationClaim *claim);
    void release_invalidation_claim(ExactInvalidationClaim *claim);

    static_assert(std::atomic<Generation>::is_always_lock_free);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint8_t>::is_always_lock_free);
    std::atomic<std::uint32_t> publication_sequence_{0};
    std::atomic<Generation> generation_{0};
    std::atomic<std::uint8_t> desired_{static_cast<std::uint8_t>(OutputRoute::kNone)};
    std::atomic<std::uint8_t> active_{static_cast<std::uint8_t>(OutputRoute::kNone)};
    std::atomic<std::uint8_t> transition_{static_cast<std::uint8_t>(Transition::kStable)};
    std::atomic_bool writer_active_{false};
    static constexpr ConditionalInvalidationToken kConditionalPublishing =
        std::numeric_limits<ConditionalInvalidationToken>::max();
    static constexpr std::uint8_t kDurableNone = 0;
    static constexpr std::uint8_t kDurablePublishing = 1;
    static constexpr std::uint8_t kDurableActive = 2;
    std::atomic<ConditionalInvalidationToken> conditional_token_{
        kNoConditionalInvalidationToken};
    std::atomic<Generation> conditional_generation_{0};
    std::atomic<ConditionalInvalidationToken> next_conditional_token_{1};
    std::atomic<Generation> durable_generation_{0};
    std::atomic<std::uint8_t> durable_state_{kDurableNone};
    static constexpr UsbPublicationCut kUsbPublicationSerialMask =
        (UsbPublicationCut{1} << 30U) - 1U;
    static constexpr UsbPublicationCut kUsbPublicationActive =
        UsbPublicationCut{1} << 30U;
    static constexpr UsbPublicationCut kUsbPublicationVeto =
        UsbPublicationCut{1} << 31U;
    std::atomic<UsbPublicationCut> usb_publication_state_{0};
#ifdef HID_ROUTE_NATIVE_TEST
    GenerationPublishedHook generation_published_hook_ = nullptr;
    WriterAcquiredHook writer_acquired_hook_ = nullptr;
    WriterAcquiredHook usb_publication_before_release_hook_ = nullptr;
    WriterAcquiredHook usb_publication_after_release_hook_ = nullptr;
#endif
};

}  // namespace hid_route
