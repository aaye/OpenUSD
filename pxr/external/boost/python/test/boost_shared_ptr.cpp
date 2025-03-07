//
// Copyright 2024 Pixar
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
// Copyright David Abrahams 2002.
// Copyright Stefan Seefeld 2016.
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
#define PXR_BOOST_PYTHON_HAS_BOOST_SHARED_PTR

#include "pxr/external/boost/python/module.hpp"
#include "pxr/external/boost/python/class.hpp"
#include "pxr/external/boost/python/call_method.hpp"
#include "pxr/external/boost/python/extract.hpp"
#include "pxr/external/boost/python/def.hpp"
#include <boost/shared_ptr.hpp>
#include <memory>

using boost::shared_ptr;
#define MODULE boost_shared_ptr_ext

#include "shared_ptr.hpp"
#include "module_tail.cpp"

