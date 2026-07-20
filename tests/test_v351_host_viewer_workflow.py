import unittest

from tools.host_viewer.workflow import LiveEvidence, LiveState, LiveWorkflow


class FakeLiveAdapter:
    def __init__(self, *, preflight_result=True, start_error=None):
        self.preflight_result = preflight_result
        self.start_error = start_error
        self.calls = []

    def preflight(self):
        self.calls.append("preflight")
        return self.preflight_result

    def start(self):
        self.calls.append("start")
        if self.start_error:
            raise RuntimeError(self.start_error)
        return True

    def stop(self):
        self.calls.append("stop")
        return True


def ready_evidence(**overrides):
    values = {
        "board_status_readable": True,
        "rtsp_frame_received": True,
        "video_frame_id": 10,
        "detection_frame_id": 10,
        "alignment_state": "matched",
        "process_alive": True,
    }
    values.update(overrides)
    return LiveEvidence(**values)


class LiveWorkflowTest(unittest.TestCase):
    def test_defaults_to_idle_and_start_runs_preflight(self):
        adapter = FakeLiveAdapter()
        workflow = LiveWorkflow(adapter)

        self.assertEqual(LiveState.IDLE, workflow.state)
        token = workflow.start()

        self.assertEqual(LiveState.STARTING, workflow.state)
        self.assertEqual(["preflight", "start"], adapter.calls)
        self.assertTrue(token > 0)

    def test_failed_preflight_enters_retryable_error(self):
        adapter = FakeLiveAdapter(preflight_result=False)
        workflow = LiveWorkflow(adapter)

        workflow.start()

        self.assertEqual(LiveState.ERROR, workflow.state)
        self.assertEqual("preflight", workflow.error.code)
        self.assertIn("preflight", workflow.error.message)
        self.assertEqual(["preflight"], adapter.calls)

    def test_starting_can_be_cancelled_without_late_start_error(self):
        adapter = FakeLiveAdapter()
        workflow = LiveWorkflow(adapter)
        token = workflow.begin_start()

        self.assertEqual(LiveState.STOPPING, workflow.request_stop())
        self.assertEqual(LiveState.STOPPING, workflow.state)
        workflow.complete_start(token, "late launch failure")
        self.assertEqual(LiveState.STOPPING, workflow.state)
        workflow.complete_stop()
        self.assertEqual(LiveState.IDLE, workflow.state)

    def test_running_requires_status_rtsp_and_aligned_metadata(self):
        workflow = LiveWorkflow()
        token = workflow.begin_start()

        workflow.complete_start(token)
        workflow.observe(ready_evidence(board_status_readable=False))
        self.assertEqual(LiveState.STARTING, workflow.state)
        workflow.observe(ready_evidence(rtsp_frame_received=False))
        self.assertEqual(LiveState.STARTING, workflow.state)
        workflow.observe(ready_evidence(alignment_state="stale", detection_frame_id=9))
        self.assertEqual(LiveState.STARTING, workflow.state)
        workflow.observe(ready_evidence(alignment_state="clean"))
        self.assertEqual(LiveState.STARTING, workflow.state)
        workflow.observe(ready_evidence(alignment_state="latest"))
        self.assertEqual(LiveState.STARTING, workflow.state)
        workflow.observe(ready_evidence())
        self.assertEqual(LiveState.RUNNING, workflow.state)

    def test_starting_transient_data_failure_is_degraded(self):
        workflow = LiveWorkflow()
        token = workflow.begin_start()
        workflow.complete_start(token)

        workflow.observe(LiveEvidence(
            board_status_readable=True,
            process_alive=True,
            transient_error="RTSP read failed",
        ))

        self.assertEqual(LiveState.DEGRADED, workflow.state)

    def test_clean_rtsp_requires_current_ai_side_channel_metadata(self):
        workflow = LiveWorkflow()
        token = workflow.begin_start()
        workflow.complete_start(token)

        workflow.observe(ready_evidence(alignment_state="clean"))
        self.assertEqual(LiveState.STARTING, workflow.state)
        workflow.observe(ready_evidence(alignment_state="clean", side_channel_metadata_ready=True))
        self.assertEqual(LiveState.RUNNING, workflow.state)

    def test_runtime_data_loss_is_degraded_and_does_not_restart(self):
        adapter = FakeLiveAdapter()
        workflow = LiveWorkflow(adapter)
        token = workflow.begin_start()
        workflow.complete_start(token)
        workflow.observe(ready_evidence())

        workflow.observe(LiveEvidence(
            board_status_readable=True,
            rtsp_frame_received=False,
            process_alive=True,
            transient_error="RTSP read failed",
        ))

        self.assertEqual(LiveState.DEGRADED, workflow.state)
        self.assertEqual([], adapter.calls)

        workflow.observe(ready_evidence(transient_error="RTSP read failed"))
        self.assertEqual(LiveState.DEGRADED, workflow.state)

    def test_process_exit_enters_error_and_start_can_retry(self):
        adapter = FakeLiveAdapter()
        workflow = LiveWorkflow(adapter)
        token = workflow.begin_start()
        workflow.complete_start(token)
        workflow.observe(ready_evidence())

        workflow.observe(LiveEvidence(process_alive=False, fatal_error="stream process exited"))
        self.assertEqual(LiveState.ERROR, workflow.state)
        self.assertEqual("process-exit", workflow.error.code)

        retry_token = workflow.begin_start()
        self.assertNotEqual(token, retry_token)
        self.assertEqual(LiveState.STARTING, workflow.state)

    def test_stop_transitions_to_idle_only_after_completion(self):
        adapter = FakeLiveAdapter()
        workflow = LiveWorkflow(adapter)
        token = workflow.start()
        workflow.complete_start(token)
        workflow.observe(ready_evidence())

        workflow.request_stop()
        self.assertEqual(LiveState.STOPPING, workflow.state)
        workflow.complete_stop()
        self.assertEqual(LiveState.IDLE, workflow.state)


if __name__ == "__main__":
    unittest.main()
