// measurement_thread.hpp: asks macOS to run the calling thread on a P-core,
// and reports whether the request took.
//
// Used by every timed program: harness_a, harness_b,
// measure_condvar_wakeup, measure_pacing_floor and measure_parse_cost.
// Each refuses to report a run unless the result is `applied`.
//
// macOS has no thread pinning (no taskset, no isolcpus), and the M2's P-
// and E-cores differ enough that a thread moved mid-run changes the number.
// QOS_CLASS_USER_INTERACTIVE biases a thread toward the P-cores. It is a
// hint: it does not stop the scheduler migrating the thread, and reading
// the class back confirms the class, not the core.
//
// Its effect on variance has not been isolated. The committed A1 runs
// before and after it (30 Aug, 4 Sep) differ in date and code as well, and
// while four of their five arms narrowed, queue_seq_cst widened.
//
// The class is read back after being set because a request that silently
// did nothing is worse than not making it: the run would be reported as
// mitigated when it was not.

#pragma once

#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif


enum class QosResult {
    // Deliberately the zero value, so a QosResult that was never assigned
    // — an aggregate initialiser that omits the member, a struct built
    // with fewer initialisers than members — reads as "not attempted" and
    // fails validation, rather than silently reading as "applied".
    not_attempted,

    // Set, and read back as QOS_CLASS_USER_INTERACTIVE.
    applied,

    // The set call reported success but the class read back as something
    // else, or could not be read at all.
    mismatch,

    // The set call itself failed.
    failed,

    // Not an Apple platform; no equivalent mechanism is attempted.
    unsupported
};


inline QosResult request_user_interactive_qos() noexcept
{
#if defined(__APPLE__)
    if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) != 0) {
        return QosResult::failed;
    }

    qos_class_t observed = QOS_CLASS_UNSPECIFIED;
    int relative_priority = 0;

    if (pthread_get_qos_class_np(
            pthread_self(),
            &observed,
            &relative_priority
        ) != 0) {
        return QosResult::mismatch;
    }

    return observed == QOS_CLASS_USER_INTERACTIVE
        ? QosResult::applied
        : QosResult::mismatch;
#else
    return QosResult::unsupported;
#endif
}


inline const char* qos_result_name(QosResult result) noexcept
{
    switch (result) {
    case QosResult::not_attempted:
        return "not_attempted";
    case QosResult::applied:
        return "applied";
    case QosResult::mismatch:
        return "mismatch";
    case QosResult::failed:
        return "failed";
    case QosResult::unsupported:
        return "unsupported";
    }

        // Reached only by a value cast in from outside the enumerators.
    return "<unrecognised QosResult>";
}


// Applied only if both results are applied; otherwise the first result that
// is not. The failures have no ordering, so this reports one reason, not
// the most serious one.
inline QosResult combine_qos(QosResult a, QosResult b) noexcept
{
    return a == QosResult::applied ? b : a;
}