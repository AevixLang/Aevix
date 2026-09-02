.PHONY: setup build clean run test

ROOT := $(shell pwd)
FILE ?= examples/test_full.aev

setup:
	@echo "🔄 Setting up Python venv..."
	python3 -m venv venv
	. venv/bin/activate && pip install -r frontend/requirements.txt
	@echo "✅ Setup complete!"

build:
	@echo "🔨 Building backend..."
	cd backend/build && cmake -DLLVM_DIR="$$(brew --prefix llvm)/lib/cmake/llvm" .. && make
	@echo "✅ Backend build complete!"

run:
	@echo "🚀 Compiling and running $(FILE)..."
	cd tools && go run ./cmd/aevix -root "$(ROOT)" -run "$(FILE)"

clean:
	@echo "🧹 Cleaning..."
	cd tools && go run ./cmd/aevix -root "$(ROOT)" -clean
	@echo "✅ Clean complete!"

test:
	@echo "🧪 Running tests..."
	cd frontend && pytest tests/
