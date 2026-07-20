import unittest

from tools.host_viewer.recording import (
    RECORDING_MODES,
    RecorderState,
    RecordingPlan,
    RecordingWorkflow,
)


class RecordingPlanTest(unittest.TestCase):
    def test_modes_have_explicit_frame_policy(self):
        self.assertEqual(("off", "metadata", "sampled", "all"), RECORDING_MODES)
        self.assertEqual(
            ["--runtime-session-id", "r1"],
            RecordingPlan.create("off", runtime_session_id="r1").stream_args(),
        )

        metadata = RecordingPlan.create("metadata", runtime_session_id="r2", recording_session_id="rec2")
        self.assertEqual("none", metadata.frame_mode)
        self.assertIn("--record-analysis", metadata.stream_args())
        self.assertEqual("rec2", metadata.stream_args()[metadata.stream_args().index("--recording-session-id") + 1])

        sampled = RecordingPlan.create("sampled", runtime_session_id="r3", recording_session_id="rec3")
        self.assertEqual("sampled", sampled.frame_mode)
        self.assertIn("--record-frame-mode", sampled.stream_args())

        all_frames = RecordingPlan.create("all", runtime_session_id="r4", recording_session_id="rec4")
        self.assertEqual("all", all_frames.frame_mode)

    def test_each_plan_gets_independent_generated_ids(self):
        first = RecordingPlan.create("metadata")
        second = RecordingPlan.create("metadata")
        self.assertTrue(first.runtime_session_id)
        self.assertTrue(first.recording_session_id)
        self.assertNotEqual(first.runtime_session_id, second.runtime_session_id)
        self.assertNotEqual(first.recording_session_id, second.recording_session_id)
        self.assertNotEqual(first.runtime_session_id, first.recording_session_id)

    def test_invalid_mode_is_rejected(self):
        with self.assertRaises(ValueError):
            RecordingPlan.create("jpeg-on-demand")
        with self.assertRaises(ValueError):
            RecordingPlan.create("metadata", recording_root="")
        with self.assertRaises(ValueError):
            RecordingPlan.create("metadata", recording_session_id="../escape")


class RecordingWorkflowTest(unittest.TestCase):
    def test_disabled_recorder_does_not_block_live_stop(self):
        workflow = RecordingWorkflow(RecordingPlan.create("off"))
        self.assertEqual(RecorderState.DISABLED, workflow.state)
        self.assertFalse(workflow.request_stop(confirmed=False))
        self.assertTrue(workflow.live_can_release)

    def test_runtime_failure_is_isolated_and_stop_drains(self):
        workflow = RecordingWorkflow(RecordingPlan.create("sampled"))
        workflow.observe({"state": "recording", "session_id": "rec"})
        self.assertEqual(RecorderState.RECORDING, workflow.state)

        workflow.observe({"state": "failed", "error": "disk full"})
        self.assertEqual(RecorderState.FAILED, workflow.state)
        self.assertEqual("disk full", workflow.error)
        self.assertTrue(workflow.live_can_release)

        self.assertTrue(workflow.request_stop(confirmed=True))
        self.assertTrue(workflow.stop_requested)

    def test_stop_requires_confirmation_and_waits_for_terminal_state(self):
        workflow = RecordingWorkflow(RecordingPlan.create("metadata"))
        workflow.observe({"state": "recording", "session_id": "rec"})
        self.assertFalse(workflow.request_stop(confirmed=False))
        self.assertEqual(RecorderState.RECORDING, workflow.state)
        self.assertTrue(workflow.request_stop(confirmed=True))
        self.assertEqual(RecorderState.FINALIZING, workflow.state)
        self.assertFalse(workflow.live_can_release)

        workflow.observe({"state": "finalized", "session_id": "rec"})
        self.assertEqual(RecorderState.FINALIZED, workflow.state)
        self.assertTrue(workflow.live_can_release)

    def test_drain_timeout_is_explicitly_interrupted(self):
        workflow = RecordingWorkflow(RecordingPlan.create("all"))
        workflow.observe({"state": "recording"})
        workflow.request_stop(confirmed=True)
        workflow.mark_interrupted("drain timeout")
        self.assertEqual(RecorderState.INTERRUPTED, workflow.state)
        self.assertEqual("drain timeout", workflow.error)
        self.assertTrue(workflow.live_can_release)


if __name__ == "__main__":
    unittest.main()
