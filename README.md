# Kate Codex Panel

Kate plugin for sending one-off prompts to `codex exec` from a sidebar panel.

## What it does

- Adds a `Chat` panel and a `Config` panel to Kate.
- Sends the active file path, cursor position, and optional selection as context.
- Runs Codex as a single request per prompt, with no background agent.
- Can request structured edits and apply them back to the current document.
- Stores profiles and settings locally in `~/.config/katecodexpanelrc`.

## Requirements

- Kate with KF6 support.
- `cmake`, a C++ toolchain, Qt6, and KDE Frameworks 6 development packages.
- The `codex` CLI installed and authenticated.
- A Codex command that works non-interactively from the terminal.

## Build

```bash
cmake -B build
cmake --build build
```

## Install

There are two installation modes:

- Local install: copies the plugin into `~/.local` and is enough if your Kate session already sees local Qt plugin paths.
- System install: copies the plugin into the Kate/KF6 plugin directory under `/usr/lib`, which makes it available globally without relying on environment variables.

### Local install

```bash
cmake --install build --prefix "$HOME/.local"
```

### System install

```bash
sudo ./install.sh
```

Use this if you want the plugin to be available in Kate without `QT_PLUGIN_PATH` or any custom launcher.

## Enable in Kate

1. Restart Kate completely.
2. Open `Settings -> Configure Kate -> Plugins`.
3. Enable `Kate Codex Panel`.
4. Open the panel from the right sidebar icon.

## Usage

1. Open a file in Kate.
2. Write a question in the `Chat` panel.
3. Optionally select text before sending to include that selection in the context.
4. Press `Send`.

If `Allow structured edits` is enabled in the config panel, Codex can return structured edits that the plugin applies to the saved file.

## Configuration

The `Config` panel lets you adjust:

- the base Codex command
- the system prompt
- whether to send chat history
- whether structured edits are allowed
- context size limits
- highlight color for applied edits
- reusable profiles with a default profile

## Notes

- The plugin saves the current document before sending a request.
- Codex reads the saved file from disk using the file path as source of truth.
- The visible conversation log is local to the panel.
- If Kate does not show the plugin after installation, fully close all Kate windows and start it again.
