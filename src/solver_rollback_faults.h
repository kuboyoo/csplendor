#ifndef CSPLENDOR_SOLVER_ROLLBACK_FAULTS_H
#define CSPLENDOR_SOLVER_ROLLBACK_FAULTS_H

// Test-only seam. Entirely absent from deployment code: not a public callback
// and not a way to turn transition failures into REFUTED results.
#ifdef CSPLENDOR_VERIFY_SOLVER_ROLLBACK
namespace csplendor::solver_internal {
enum class RollbackFaultPoint {
  PaymentColour, ProvenanceAppend, SourceRemoval, NobleAcquired,
  BeforeHashCommit, AfterHashCommit
};
inline thread_local void (*rollback_fault_hook)(RollbackFaultPoint) = nullptr;
inline void rollback_fault(RollbackFaultPoint point) {
  if (rollback_fault_hook) rollback_fault_hook(point);
}
} // namespace csplendor::solver_internal
#define CSPLENDOR_ROLLBACK_FAULT(point) \
  ::csplendor::solver_internal::rollback_fault( \
      ::csplendor::solver_internal::RollbackFaultPoint::point)
#else
#define CSPLENDOR_ROLLBACK_FAULT(point) do {} while (false)
#endif

#endif
