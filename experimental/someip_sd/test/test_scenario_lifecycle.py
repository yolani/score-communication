# *******************************************************************************
# Copyright (c) 2026 Contributors to the Eclipse Foundation
#
# See the NOTICE file(s) distributed with this work for additional
# information regarding copyright ownership.
#
# This program and the accompanying materials are made available under the
# terms of the Apache License Version 2.0 which is available at
# https://www.apache.org/licenses/LICENSE-2.0
#
# SPDX-License-Identifier: Apache-2.0
# *******************************************************************************
"""Host-side checks of platform selection, readiness and failure cleanup."""

import unittest
from unittest.mock import Mock, patch

import someip_sd_scenario as scenario


class ScenarioLifecycleTest(unittest.TestCase):
    def test_qnx_loads_module_and_waits_for_readiness(self):
        console = Mock()
        console.run_sh_cmd_output.side_effect = [(0, ""), (0, "ready: kernel socket event module")]
        side = scenario.ConsoleSide(console)
        side.start_event_module()
        calls = console.run_sh_cmd_output.call_args_list
        self.assertIn("mount -T io-sock /opt/someip_sd/bin/mods-someip-event.so", calls[0].args[0])
        self.assertIn("cat /tmp/someip_sd_event_module.log", calls[1].args[0])
        self.assertFalse(any("someip_event_module " in call.args[0] for call in calls))

    def test_qnx_readiness_failure_is_reported(self):
        console = Mock()
        console.run_sh_cmd_output.return_value = (0, "")
        with patch.object(scenario.time, "monotonic", side_effect=[0, 11]):
            with self.assertRaisesRegex(RuntimeError, "did not become ready"):
                scenario.ConsoleSide(console).start_event_module()

    def test_linux_starts_original_binary(self):
        container = Mock()
        scenario.ContainerSide(container).start_event_module()
        command = container.exec_run.call_args.args[0][-1]
        self.assertIn("/bin/someip_event_module /opt/someip_sd/etc/mw_com_config.json", command)
        self.assertNotIn("mount", command)
        self.assertTrue(container.exec_run.call_args.kwargs["detach"])

    def test_qnx_tracks_and_cleans_only_owned_processes(self):
        console = Mock()
        console.run_sh_cmd_output.return_value = (0, "")
        side = scenario.ConsoleSide(console)
        side.start("daemon > /tmp/daemon.log 2>&1")
        side.cleanup()
        calls = console.run_sh_cmd_output.call_args_list
        self.assertIn("echo $! > /tmp/someip_sd_process_0.pid", calls[0].args[0])
        self.assertIn('kill -TERM "$pid"', calls[1].args[0])
        self.assertNotIn("umount", calls[1].args[0])
        self.assertNotIn("io-sock", calls[1].args[0])

    def test_both_sides_are_cleaned_on_success(self):
        sd, peer = Mock(), Mock()
        with patch.object(scenario, "_run"):
            scenario.run(sd, peer)
        sd.cleanup.assert_called_once()
        peer.cleanup.assert_called_once()

    def test_cleanup_preserves_primary_failure(self):
        sd, peer = Mock(), Mock()
        sd.cleanup.side_effect = RuntimeError("cleanup error")
        with patch.object(scenario, "_run", side_effect=AssertionError("scenario error")):
            with self.assertLogs(scenario.logger, level="ERROR"):
                with self.assertRaisesRegex(AssertionError, "scenario error"):
                    scenario.run(sd, peer)
        peer.cleanup.assert_called_once()

    def test_cleanup_failure_fails_successful_scenario(self):
        sd, peer = Mock(), Mock()
        sd.cleanup.side_effect = RuntimeError("cleanup error")
        with patch.object(scenario, "_run"):
            with self.assertLogs(scenario.logger, level="ERROR"):
                with self.assertRaisesRegex(RuntimeError, "cleanup error"):
                    scenario.run(sd, peer)
        peer.cleanup.assert_called_once()


if __name__ == "__main__":
    unittest.main()
