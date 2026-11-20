# Overleaf Report for xv6 Hierarchical Inode FUSE

## How to use this project in Overleaf:
1. Open [Overleaf](https://www.overleaf.com/).
2. Click **New Project** -> **Upload Project**.
3. Select the provided `overleaf_report.zip` file.
4. Overleaf will automatically open and compile `main.tex` into a publication-grade PDF report!

## Included Features in the LaTeX Document:
- **Title Page & Abstract**: Formatted for university submission.
- **Pure TikZ Vector Diagrams (No external images required)**:
  - **Figure 1**: End-to-end request/reply control flow diagram from user app to kernel to FUSE daemon.
  - **Figure 2**: Hierarchical Inode Tree structure diagram illustrating parent-child pointers.
  - **Figure 3**: 8-slot IPC queue state machine diagram (`FREE`, `FILL`, `PENDING`, `INFLIGHT`, `DONE`, `ABANDON`).
- **Complete Code Listings**: Formatted syntax-highlighted snippets for `struct inode`, `path_lookup`, and `sys_mkdir` hook.
- **Experimental Verification Trace**: The exact terminal trace proving `MKDIR`, `OPEN`, `READ`, `WRITE` and `fusetest ALL PASSED`.
- **Comparative Analysis Table**: Highlighting original vs upgraded project capabilities.
