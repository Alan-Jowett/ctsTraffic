All C and C++ source, header files, classes, functions, methods, macros, enums, constants, fields, and properties in this repository must include Doxygen-style documentation.

Purpose
- Ensure consistent, useful API documentation across the codebase.

Scope
- Applies to all files in the repository with C or C++ source or header extensions (for example: `.c`, `.cpp`, `.cc`, `.cxx`, `.h`, `.hpp`, `.hh`).

Requirements
- Every file: include a file-level Doxygen comment block describing the file purpose, authorship, and copyright.
  - If a file already contains a copyright notice (top-of-file), merge that notice into the file-level Doxygen block instead of duplicating it.
- Every class, struct, enum, namespace, and union: add a Doxygen block immediately above the declaration that clearly explains its purpose and public behavior.
- Every function and method: add a Doxygen block immediately above the declaration that includes:
  - A short one-line summary.
  - A longer description if needed.
  - `@param` tags for each parameter. Use attribute qualifiers for parameter direction: `@param[in]`, `@param[out]`, or `@param[in,out]` as appropriate.
  - `@return` (or `@retval`) describing the return value when not `void`.
  - `@throws` or `@exception` if the function can throw exceptions (C++ only).
  - `@remarks` or `@note` for any important implementation details, side-effects, or thread-safety notes.
- Every macro, constant, and typedef: include a short Doxygen comment describing purpose and semantics.

Formatting and style
- Use Doxygen "triple-slash" (`///`) or block (`/** ... */`) style consistently within a file; prefer the style already used in the file when merging.
- Use `@param[in]`, `@param[out]`, or `@param[in,out]` on every `@param` line. Prefer `@param[in,out]` when a parameter is both read and written.
- Keep the one-line summary under 80 characters when possible and the rest of the description wrapped at approximately 100 characters.

Merging existing comments
- When adding Doxygen to an existing symbol, if there are existing informal comments immediately above the symbol (for example `//`, `/* */`, or older block comments):
  - Preserve the original text and fold it into the new Doxygen block. Reformat the text to comply with Doxygen syntax but do not remove factual content.
  - If the existing comment includes copyright or license text, place that content into the file-level Doxygen block and remove duplicate text from lower-level comments.

Practical examples
- For functions, always annotate parameter direction. Example:

  /**
   * Opens a connection to the server and returns a socket handle.
   *
   * @param[in] serverName Null-terminated server name string.
   * @param[in,out] timeoutMs Pointer to a timeout in milliseconds. On return may be updated with remaining time.
   * @return socket handle on success, INVALID_SOCKET on failure.
   */

Automation guidance for copilot / code assistants
- When prompted to insert or update documentation, generate a Doxygen block that follows the requirements above.
- If the file already has comments, merge them as described — do not create duplicate or conflicting copyright lines.
- Prefer `@param[in,out]` rather than combining separate `@in` / `@out` notes inside the description.

Review and enforcement
- During code review, maintainers should ensure all new and modified C/C++ symbols follow these rules.
- Missing documentation or incorrect `@param` direction should be requested as changes during review.

Relation to other repository docs
- This file adds guidance specifically for Copilot/code-assist behavior. It does not replace project-level contribution or license files; it complements them by specifying documentation expectations.

Contact
- If you have questions about the policy or need an exception, open an issue and tag the maintainers.
