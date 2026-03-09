OpenSCAD AI Build for Windows
=============================

Launch
- Run openscad.exe from this folder.
- Keep the entire folder together. The Qt plugins, shaders, color schemes, and TLS backends beside the exe are required.

AI Setup
- OpenAI: enter an API key in the AI panel.
- Gemini: use a Google AI Studio API key for the simplest setup.
- Advanced Gemini OAuth is available, but it still requires a Google Desktop OAuth client JSON and billed Google Cloud project details.

Credential Storage
- OpenAI and Gemini secrets are stored in Windows Credential Manager for this user account.
- They are no longer kept in plain text in OpenSCAD's normal settings entries.

Export
- STL, CSG, and PNG export dialogs now append the expected file extension when it is omitted.

Notes
- This is a portable app folder built from the modified OpenSCAD source tree.
- If you move this build to another Windows machine, that machine may also need the Microsoft Visual C++ runtime installed.
