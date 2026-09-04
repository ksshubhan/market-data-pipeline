#pragma once

#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif


// §5: macOS offers no thread pinning — no taskset, no isolcpus — and the
// M2's heterogeneous P/E cores mean the scheduler can migrate a
// measurement thread mid-run. pthread_set_qos_class_self_np with
// QOS_CLASS_USER_INTERACTIVE biases a thread toward the P-cores.
//
// This is a hint, not a guarantee, and must be described that way. It
// reduces run-to-run variance; it does not eliminate it, and it does not
// prevent migration.
//
// The class is read back after being set, because a request that silently
// did nothing is worse than not making it: the run would be reported as
// mitigated when it was not.

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

    return "<unrecognised QosResult>";
}


// Worst-of, for combining the producer's and consumer's results into one
// per-trial verdict. `applied` is the only acceptable outcome.
inline QosResult combine_qos(QosResult a, QosResult b) noexcept
{
    return a == QosResult::applied ? b : a;
}