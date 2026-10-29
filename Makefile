CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2 -Iinclude
LDFLAGS  ?=
EXE      :=

ifeq ($(OS),Windows_NT)
    LDFLAGS += -lws2_32
    EXE     := .exe
    PYTHON  ?= python
else
    PYTHON  ?= python3
endif

SRC      := $(wildcard src/*.cpp)
OBJ      := $(SRC:src/%.cpp=build/%.o)
OBJ_LIB  := $(filter-out build/main.o, $(OBJ))

all: minigit$(EXE)

minigit: minigit$(EXE)

minigit$(EXE): $(OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

build/%.o: src/%.cpp $(wildcard include/*.h) | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build:
	@$(PYTHON) -c "import os; os.makedirs('build', exist_ok=True)"

ui:
	$(PYTHON) tools/embed_frontend.py

build/test_fixes$(EXE): tests/test_fixes.cpp $(OBJ_LIB) | build
	$(CXX) $(CXXFLAGS) $< $(OBJ_LIB) -o $@ $(LDFLAGS)

build/test_p2p$(EXE): tests/test_p2p.cpp $(OBJ_LIB) | build
	$(CXX) $(CXXFLAGS) $< $(OBJ_LIB) -o $@ $(LDFLAGS)

build/test_cli_server$(EXE): tests/test_cli_server.cpp $(OBJ_LIB) | build
	$(CXX) $(CXXFLAGS) $< $(OBJ_LIB) -o $@ $(LDFLAGS)

build/test_core_fixes$(EXE): tests/test_core_fixes.cpp $(OBJ_LIB) | build
	$(CXX) $(CXXFLAGS) $< $(OBJ_LIB) -o $@ $(LDFLAGS)

TEST_TARGETS := build/test_fixes$(EXE) build/test_p2p$(EXE) build/test_cli_server$(EXE) build/test_core_fixes$(EXE)

test: $(TEST_TARGETS)
	./build/test_fixes$(EXE)
	./build/test_p2p$(EXE)
	./build/test_cli_server$(EXE)
	./build/test_core_fixes$(EXE)

clean:
	@$(PYTHON) -c "import shutil, os, glob; shutil.rmtree('build', ignore_errors=True); [os.remove(f) for f in glob.glob('minigit*') if f in ('minigit', 'minigit.exe') and os.path.isfile(f)]"

.PHONY: all minigit clean test ui
