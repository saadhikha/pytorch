#include <torchpy.h>
#include <assert.h>
#include <pybind11/embed.h>
#include <stdio.h>
#include <torch/csrc/jit/python/pybind_utils.h>
#include <torch/csrc/autograd/generated/variable_factories.h>
#include <iostream>
#include <mutex>
#include <thread>

namespace torchpy {

PyThreadState* mainThreadState = NULL;
std::mutex mtx;

void init() {
  std::thread::id this_id = std::this_thread::get_id();
  std::cout << "init: thread id " << this_id << std::endl;

  Py_Initialize();
  // Make sure torch is loaded before anything else
  py::module::import("torch");

  // Why does importing it here not obviate the need to import it in loader.py?
  // py::exec("from torch import Tensor");

  // Make loader importable
  py::exec(R"(
        import sys
        sys.path.append('torchpy')
    )");

  // Enable other threads to use the interpreter
  assert(PyGILState_Check() == 1);
  assert(PyEval_ThreadsInitialized() != 0);
  mainThreadState = PyEval_SaveThread(); // save our state, release GIL
}

void finalize() {
  PyEval_RestoreThread(mainThreadState); // Acquire GIL, resume our state
  Py_Finalize();
}
const PyModule load(const char* filename) {
  std::cout << "load()" << std::endl;
  PyEval_RestoreThread(mainThreadState); // Acquire GIL, resume our state
  assert(PyGILState_Check() == 1);

  auto loader = py::module::import("loader");
  auto load = loader.attr("load");

  std::cout << "  callobject load" << std::endl;
  auto model = load(filename);
  auto mod = PyModule(model);

  mainThreadState = PyEval_SaveThread(); // save our state, release GIL
  std::cout << "load return" << std::endl;
  return mod;
}

PyModule::PyModule(py::object model) : _model(model) {}
// make sure not to destroy python objects in here as
// py finalize may have happened before these modules
// get destroyed.
PyModule::~PyModule() {
}

at::Tensor PyModule::forward(at::Tensor input) {
  at::Tensor output;
  mtx.lock();
  std::thread::id this_id = std::this_thread::get_id();
  std::cout << "forward: thread id " << this_id << std::endl;
  PyInterpreterState* mainInterpreterState = mainThreadState->interp;
  PyThreadState* myThreadState =
      PyThreadState_New(mainInterpreterState); // GIL not needed
  PyEval_RestoreThread(myThreadState);
  {
    py::object forward = _model.attr("forward");
    std::cout << "  called forward" << std::endl;
    py::object py_output = forward(input);
    std::cout << "  casting output" << std::endl;
    output = std::move(py::cast<at::Tensor>(py_output));
    PyThreadState_Clear(myThreadState); // GIL needed
  }
  PyEval_ReleaseThread(myThreadState); // Releases GIL
  PyThreadState_Delete(myThreadState); // GIL not needed
  std::cout << "sanitizing output" << std::endl;
  at::Tensor new_output = torch::ones_like(output); 
  // new_output.copy_(output.value());
  // at::Tensor new_output = output.clone();
  std::cout << "returning output" << std::endl;
  mtx.unlock();
  return new_output;
}
} // namespace torchpy