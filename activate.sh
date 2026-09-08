#!/bin/bash
source venv/bin/activate

# Keep the aevix CLI on PATH (symlink into ~/.local/bin so it works from any dir).
mkdir -p "$HOME/.local/bin"
ln -sf "$(pwd)/aevix" "$HOME/.local/bin/aevix"
export PATH="$HOME/.local/bin:$PATH"

echo "✅ Aevix environment activated!"
echo "📁 Python: $(which python3)"
echo "📁 Go: $(which go)"
echo "📁 aevix: $(which aevix)"