#!/bin/bash
source venv/bin/activate
export PATH=$PWD/tools:$PATH
echo "✅ Aevix environment activated!"
echo "📁 Python: $(which python3)"
echo "📁 Go: $(which go)"