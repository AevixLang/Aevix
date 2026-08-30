.PHONY: setup build clean run test

setup:
	@echo "🔄 Setting up Python venv..."
	python3 -m venv venv
	. venv/bin/activate && pip install -r frontend/requirements.txt
	@echo "✅ Setup complete!"

build:
	@echo "🔨 Building..."
	cd backend && mkdir -p build && cd build && cmake .. && make
	@echo "✅ Build complete!"

run: build
	@echo "🚀 Running program..."
	cd tools && go run cmd/velo/main.go -run

clean:
	@echo "🧹 Cleaning..."
	rm -rf venv
	rm -rf backend/build
	rm -f program output.ll output.o ast.json
	@echo "✅ Clean complete!"

test:
	@echo "🧪 Running tests..."
	cd frontend && pytest tests/