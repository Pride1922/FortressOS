#!/usr/bin/env python3
"""Capture-boundary regression; no QEMU or guest execution."""
import unittest
from test_shell_s6 import ready_prompt


class PromptBoundaryTests(unittest.TestCase):
    def test_long_pipeline_redraws_are_not_completion(self):
        echo = "/bin/pipetest produce | /bin/pipetest relay | "
        redraws = echo + ("fortress> " + echo) * 8
        for end in range(len(redraws) + 1):
            self.assertIsNone(ready_prompt(redraws[:end], submitted=True))
        self.assertIsNone(ready_prompt(redraws + "\n", submitted=True))
        self.assertIsNone(ready_prompt(redraws + "\nPIPELINE BYTES OK\nfortress>", submitted=True))
        capture = redraws + "\nPIPELINE BYTES OK\nfortress> "
        prompt = ready_prompt(capture, submitted=True)
        self.assertIsNotNone(prompt)
        self.assertEqual(capture[capture.index("\n") + 1:prompt.start()], "PIPELINE BYTES OK\n")

    def test_silent_command(self):
        capture = "echo hidden > /mnt/out\nfortress> "
        prompt = ready_prompt(capture, submitted=True)
        self.assertIsNotNone(prompt)
        self.assertEqual(capture[capture.index("\n") + 1:prompt.start()], "")

    def test_prompt_inside_output_is_not_the_final_boundary(self):
        capture = "cat /mnt/out\nfortress> embedded output\nmore\nfortress> "
        prompt = ready_prompt(capture, submitted=True)
        self.assertEqual(prompt.start(), capture.rfind("fortress> "))
        self.assertIsNone(ready_prompt("command\nfortress> still running\n", submitted=True))

    def test_initial_and_cwd_prompts(self):
        self.assertIsNotNone(ready_prompt("boot\nfortress> "))
        self.assertIsNone(ready_prompt("fortress> ", submitted=True))
        self.assertIsNotNone(ready_prompt("false\n[1] fortress:/mnt $ ", submitted=True))


if __name__ == "__main__":
    unittest.main()
