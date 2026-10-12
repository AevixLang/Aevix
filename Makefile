.PHONY: setup build build-frontend clean run test

ROOT := $(shell pwd)
FILE ?= examples/test_full.aev

setup:
	@echo "🔄 Setting up Python venv..."
	python3 -m venv venv
	. venv/bin/activate && pip install -r frontend/requirements.txt
	@echo "✅ Setup complete!"

build:
	@echo "🔨 Building backend..."
	mkdir -p backend/build
	cd backend/build && cmake .. -DCMAKE_BUILD_TYPE=Release && make
	@echo "✅ Backend build complete!"

build-frontend:
	@echo "🔨 Building C++ frontend..."
	mkdir -p frontend/cpp/build
	cd frontend/cpp/build && cmake .. -DCMAKE_CXX_COMPILER=clang++ && make -j4
	cp frontend/cpp/build/aevix-frontend backend/build/
	@echo "✅ Frontend build complete!"

run:
	@echo "🚀 Compiling and running $(FILE)..."
	./aevix -root "$(ROOT)" run "$(FILE)"

clean:
	@echo "🧹 Cleaning..."
	./aevix -root "$(ROOT)" clean
	@echo "✅ Clean complete!"

test:
	@echo "🧪 Running tests..."
	./aevix -root "$(ROOT)" selftest
