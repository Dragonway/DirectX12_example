#pragma once

#include <windows.h>
#include <wrl/client.h>

#if defined(CreateWindow)
#undef CreateWindow
#endif

#include <directx/d3d12.h>
#include <directx/d3dx12.h>
#include <directx/d3d12compiler.h>
#include <dxgi1_6.h>
#include <DirectXMath.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <memory>
