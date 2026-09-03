# TinyLSM development instructions

## C++ code style

- In long functions, separate distinct logical phases with one blank line.
- Typical phases include validation, object initialization, resource acquisition,
  state loading or creation, recovery, publication, and cleanup.
- Do not add blank lines after every statement; add them only at semantic
  boundaries.
- If blank lines are no longer enough to make a function easy to follow,
  suggest extracting cohesive helper functions instead of adding excessive
  comments or spacing.

## Output

- Codex CLI 对话中不要使用 Mermaid；需要图时使用简洁的 ASCII/Unicode 图。
- 写入 Markdown 文件时可以使用 Mermaid。

## Documentation maintenance

- After a substantial code change, proactively review and update the root
  `README.md` in the same task so it remains consistent with the implementation.
- Treat changes to the public API, CLI, build or run workflow, module boundaries,
  persistent formats, recovery or durability behavior, implemented features, or
  known limitations as substantial documentation changes.
- Do not edit `README.md` for a purely internal refactor when its documented
  behavior and architecture remain unchanged.
