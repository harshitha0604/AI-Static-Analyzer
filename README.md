AI-Driven MISRA-C Compliance Verification and Repair Using LLM

📌 Overview

Maintaining MISRA-C compliance in large and complex C programs can be challenging when developers have to manually identify where a violation occurs, understand the reason behind it, and determine how to fix it. Manually searching through large codebases can be time-consuming and may make it difficult to quickly trace the exact source of a problem.

This project addresses this challenge by developing an AI-driven static analysis system for detecting, analyzing, and repairing MISRA-C violations.

The system uses Clang/LLVM and Clang AST to perform deeper analysis of C programs and identify potential MISRA-C violations. The detected violations are then passed to an LLM through Ollama, which analyzes the issue, explains the violation, and provides possible code repair suggestions.

By combining compiler-based static analysis with AI-assisted reasoning, the system helps developers locate problems faster, understand violations more easily, and accelerate the code recovery and repair process, reducing the effort required for manual code inspection.


🎯 Problem Statement

Manual identification of MISRA-C violations can become difficult and time-consuming, especially when working with large C codebases.

A developer may need to:

1. Search through the source code to locate the problem.
2. Understand the exact reason for the violation.
3. Determine which MISRA-C rule is involved.
4. Analyze the surrounding code and program structure.
5. Identify an appropriate correction.
6. Manually modify and verify the code.

This project aims to automate and assist these steps by providing deeper code analysis, faster violation localization, AI-assisted explanation, and repair suggestions.

💡 Core Idea

Instead of manually searching through the code to find where and why a MISRA-C violation occurs, the system dives deeper into the program structure, identifies potential problem locations, analyzes the violations, and provides AI-assisted repair suggestions.

This helps achieve faster problem identification and quicker recovery/repair of coding-rule violations.
