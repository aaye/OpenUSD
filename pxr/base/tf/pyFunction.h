//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_BASE_TF_PY_FUNCTION_H
#define PXR_BASE_TF_PY_FUNCTION_H

#include "pxr/pxr.h"

#include "pxr/base/tf/pyCall.h"
#include "pxr/base/tf/pyLock.h"
#include "pxr/base/tf/pyObjWrapper.h"
#include "pxr/base/tf/pyUtils.h"

#include "pxr/external/boost/python/converter/from_python.hpp"
#include "pxr/external/boost/python/converter/registered.hpp"
#include "pxr/external/boost/python/converter/rvalue_from_python_data.hpp"
#include "pxr/external/boost/python/extract.hpp"
#include "pxr/external/boost/python/handle.hpp"
#include "pxr/external/boost/python/object.hpp"

#include <functional>

PXR_NAMESPACE_OPEN_SCOPE

template <typename T>
struct TfPyFunctionFromPython;

template <typename Ret, typename... Args>
struct TfPyFunctionFromPython<Ret (Args...)>
{
    struct Call
    {
        TfPyObjWrapper callable;

        Ret operator()(Args... args) {
            TfPyLock lock;
            return TfPyCall<Ret>(callable)(args...);
        }
    };

    struct CallWeak
    {
        TfPyObjWrapper weak;

        Ret operator()(Args... args) {
            using namespace pxr_boost::python;
            // Attempt to get the referenced callable object.
            TfPyLock lock;
            object callable(handle<>(borrowed(PyWeakref_GetObject(weak.ptr()))));
            if (TfPyIsNone(callable)) {
                TF_WARN("Tried to call an expired python callback");
                return Ret();
            }
            return TfPyCall<Ret>(callable)(args...);
        }
    };

    struct CallMethod
    {
        TfPyObjWrapper func;
        TfPyObjWrapper weakSelf;

        Ret operator()(Args... args) {
            using namespace pxr_boost::python;
            // Attempt to get the referenced self parameter, then build a new
            // instance method and call it.
            TfPyLock lock;
            PyObject *self = PyWeakref_GetObject(weakSelf.ptr());
            if (self == Py_None) {
                TF_WARN("Tried to call a method on an expired python instance");
                return Ret();
            }
            object method(handle<>(PyMethod_New(func.ptr(), self)));
            return TfPyCall<Ret>(method)(args...);
        }
    };

    TfPyFunctionFromPython() {
        RegisterFunctionType<std::function<Ret (Args...)>>();
    }

    template <typename FuncType>
    static void
    RegisterFunctionType() {
        using namespace pxr_boost::python;
        converter::registry::
            insert(&convertible, &construct<FuncType>, type_id<FuncType>());
    }

    static void *convertible(PyObject *obj) {
        return ((obj == Py_None) || PyCallable_Check(obj)) ? obj : 0;
    }

    template <typename FuncType>
    static void construct(PyObject *src, pxr_boost::python::converter::
                          rvalue_from_python_stage1_data *data) {
        using std::string;
        using namespace pxr_boost::python;
        
        void *storage = ((converter::rvalue_from_python_storage<FuncType> *)
                         data)->storage.bytes;

        if (src == Py_None) {
            new (storage) FuncType();
        } else {

            // In the case of instance methods, holding a strong reference will
            // keep the bound 'self' argument alive indefinitely, which is
            // undesirable. Unfortunately, we can't just keep a weak reference to
            // the instance method, because python synthesizes these on-the-fly.
            // Instead we do something like what PyQt's SIP does, and break the
            // method into three parts: the class, the function, and the self
            // parameter.  We keep strong references to the class and the
            // function, but a weak reference to 'self'.  Then at call-time, if
            // self has not expired, we build a new instancemethod and call it.
            //
            // Otherwise if the callable is a lambda (checked in a hacky way, but
            // mirroring SIP), we take a strong reference.
            // 
            // For all other callables, we attempt to take weak references to
            // them. If that fails, we take a strong reference.
            //
            // This is all sort of contrived, but seems to have the right behavior
            // for most usage patterns.
            
            object callable(handle<>(borrowed(src)));
            PyObject *pyCallable = callable.ptr();
            PyObject *self =
                PyMethod_Check(pyCallable) ?
                PyMethod_GET_SELF(pyCallable) : NULL;

            if (self) {
                // Deconstruct the method and attempt to get a weak reference to
                // the self instance.
                object func(handle<>(borrowed(PyMethod_GET_FUNCTION(
                                                  pyCallable))));
                object weakSelf(handle<>(PyWeakref_NewRef(self, NULL)));
                new (storage)
                    FuncType(CallMethod{
                            TfPyObjWrapper(func),
                                TfPyObjWrapper(weakSelf)
                        });
                
            } else if (PyObject_HasAttrString(pyCallable, "__name__") &&
                       extract<string>(callable.attr("__name__"))()
                                                                == "<lambda>") {
                // Explicitly hold on to strong references to lambdas.
                new (storage) FuncType(Call{TfPyObjWrapper(callable)});
            } else {
                // Attempt to get a weak reference to the callable.
                if (PyObject *weakCallable =
                    PyWeakref_NewRef(pyCallable, NULL)) {
                    new (storage)
                        FuncType(
                            CallWeak{TfPyObjWrapper(
                                    object(handle<>(weakCallable)))});
                } else {
                    // Fall back to taking a strong reference.
                    PyErr_Clear();
                    new (storage) FuncType(Call{TfPyObjWrapper(callable)});
                }
            }
        }

        data->convertible = storage;
    }
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_BASE_TF_PY_FUNCTION_H
