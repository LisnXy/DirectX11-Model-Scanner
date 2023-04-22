#include "includes.h"
#include "loguru.hpp"
#include <cstdio>
#include <unordered_set>
#include <mutex>
#include <D3Dcompiler.h>

UINT currentIndex = 0;

struct propertiesModel
{
	UINT stride;
	UINT vedesc_ByteWidth;
	UINT indesc_ByteWidth;
	UINT pscdesc_ByteWidth;

	bool operator==(const propertiesModel& other) const
	{
		return stride == other.stride && vedesc_ByteWidth == other.vedesc_ByteWidth
			&& indesc_ByteWidth == other.indesc_ByteWidth && pscdesc_ByteWidth == other.pscdesc_ByteWidth;
	}

	void print()
	{
		LOG_F(ERROR, "%d : %d : %d : %d, current:", stride, vedesc_ByteWidth, indesc_ByteWidth, pscdesc_ByteWidth, currentIndex);
	}
};

namespace std
{
	template<> struct hash<propertiesModel>
	{
		std::size_t operator()(const propertiesModel& obj) const noexcept
		{
			std::size_t h1 = std::hash<int>{}(obj.stride);
			std::size_t h2 = std::hash<int>{}(obj.vedesc_ByteWidth);
			std::size_t h3 = std::hash<int>{}(obj.indesc_ByteWidth);
			std::size_t h4 = std::hash<int>{}(obj.pscdesc_ByteWidth);
			return (h1 ^ h3 + h4) ^ (h2 << 1);
		}
	};
}

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
HRESULT GenerateShader(ID3D11Device* pD3DDevice, ID3D11PixelShader** pShader, float r, float g, float b);

Present oPresent;
DrawIndexed oDrawIndexed;
HWND window = NULL;
WNDPROC oWndProc;
ID3D11Device* pDevice = NULL;
ID3D11DeviceContext* pContext = NULL;
ID3D11RenderTargetView* mainRenderTargetView;
std::mutex mtx_models;
std::unordered_set<propertiesModel> Models;
// current model
propertiesModel currentModel{ 0,0,0,0 };
// used to render the target
ID3D11PixelShader* pShaderRed = nullptr;
ID3D11Texture2D* textureRed = nullptr;
ID3D11ShaderResourceView* textureView;
// ImGui configuration
UINT stride_min = 0;
UINT stride_max = 128;
UINT vedesc_ByteWidth_min = 0;
UINT vedesc_ByteWidth_max = 10000;
UINT indesc_ByteWidth_min = 2;
UINT indesc_ByteWidth_max = 4;
UINT pscdesc_ByteWidth_min = 16;
UINT pscdesc_ByteWidth_max = 4090;
// essential properties of a model
static UINT stride = 0;
static UINT vedesc_ByteWidth = 0;
static UINT indesc_ByteWidth = 0;
static UINT pscdesc_ByteWidth = 0;
// vertex buffer
static ID3D11Buffer* veBuffer;
static UINT veBufferOffset = 0;
static D3D11_BUFFER_DESC vedesc;
// index buffer
static ID3D11Buffer* inBuffer;
DXGI_FORMAT inFormat;
UINT inOffset;
D3D11_BUFFER_DESC indesc;
// psgetConstantbuffers
UINT pscStartSlot;
UINT pscNumBuffers;
ID3D11Buffer* pscBuffer;
D3D11_BUFFER_DESC pscdesc;

void InitImGui()
{
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags = ImGuiConfigFlags_NoMouseCursorChange;
	ImGui_ImplWin32_Init(window);
	ImGui_ImplDX11_Init(pDevice, pContext);
	LOG_F(INFO, "ImGui initialized");
}

LRESULT __stdcall WndProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (true && ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam))
		return true;

	return CallWindowProc(oWndProc, hWnd, uMsg, wParam, lParam);
}

bool init = false;
HRESULT __stdcall hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
	if (!init)
	{
		if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&pDevice)))
		{
			pDevice->GetImmediateContext(&pContext);
			DXGI_SWAP_CHAIN_DESC sd;
			pSwapChain->GetDesc(&sd);
			window = sd.OutputWindow;
			ID3D11Texture2D* pBackBuffer;
			pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
			pDevice->CreateRenderTargetView(pBackBuffer, NULL, &mainRenderTargetView);
			pBackBuffer->Release();
			oWndProc = (WNDPROC)SetWindowLongPtr(window, GWLP_WNDPROC, (LONG_PTR)WndProc);
			InitImGui();
			init = true;
		}
		else
		{
			return oPresent(pSwapChain, SyncInterval, Flags);
		}
	}

	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	ImGui::Begin("Lisn Model Scanner");
	ImGui::Text("Total model:%d, current at:%d", Models.size(), currentIndex);
	ImGui::Text("---Set the value of target Model---");
	ImGui::SliderScalar("Stride", ImGuiDataType_U32, &(currentModel.stride), &stride_min, &stride_max);
	ImGui::SliderScalar("vedesc_ByteWidth", ImGuiDataType_U32, &(currentModel.vedesc_ByteWidth), &vedesc_ByteWidth_min, &vedesc_ByteWidth_max);
	ImGui::SliderScalar("indesc_ByteWidth", ImGuiDataType_U32, &(currentModel.indesc_ByteWidth), &indesc_ByteWidth_min, &indesc_ByteWidth_max);
	ImGui::SliderScalar("pscdesc_ByteWidth", ImGuiDataType_U32, &(currentModel.pscdesc_ByteWidth), &pscdesc_ByteWidth_min, &pscdesc_ByteWidth_max);

	ImGui::End();

	ImGui::Render();

	pContext->OMSetRenderTargets(1, &mainRenderTargetView, NULL);
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

	// generate the shader
	if (pShaderRed == nullptr)
	{
		GenerateShader(pDevice, &pShaderRed, 1.0f, 0, 0);
	}
	if (textureRed == nullptr)
	{
		static const uint32_t color = 0xff0000ff;
		D3D11_SUBRESOURCE_DATA initData = { &color, sizeof(uint32_t), 0 };
		D3D11_TEXTURE2D_DESC desc;
		DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		memset(&desc, 0, sizeof(desc));
		desc.Width = 1;
		desc.Height = 1;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		HRESULT hr;
		hr = pDevice->CreateTexture2D(&desc, &initData, &textureRed);
		if (SUCCEEDED(hr) && textureRed != 0)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc;
			memset(&SRVDesc, 0, sizeof(SRVDesc));
			SRVDesc.Format = format;
			SRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			SRVDesc.Texture2D.MipLevels = 1;

			hr = pDevice->CreateShaderResourceView(textureRed, &SRVDesc, &textureView);
			if (FAILED(hr))
			{
				textureRed->Release();
				return hr;
			}
		}
	}
	return oPresent(pSwapChain, SyncInterval, Flags);
}

bool hDrawIndexedHooked = false;

void hDrawIndexed(ID3D11DeviceContext* pContext, UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation)
{
	if (!hDrawIndexedHooked)
	{
		hDrawIndexedHooked = true;
		LOG_F(INFO, "DrawIndexed hooked successfully");
	}

	// get stride & vedesc.ByteWidth
	pContext->IAGetVertexBuffers(0, 1, &veBuffer, &stride, &veBufferOffset);
	if (veBuffer)
	{
		veBuffer->GetDesc(&vedesc);
		veBuffer->Release();
		veBuffer = nullptr;
	}
	else
	{
		LOG_F(WARNING, "veBuffer is nullptr");
	}

	// get indesc.ByteWidth
	pContext->IAGetIndexBuffer(&inBuffer, &inFormat, &inOffset);
	if (inBuffer)
	{
		inBuffer->GetDesc(&indesc);
		inBuffer->Release();
		inBuffer = nullptr;
	}
	else
	{
		LOG_F(WARNING, "inBuffer is nullptr");
	}

	// get pscdesc.ByteWidth
	pContext->PSGetConstantBuffers(pscStartSlot, 1, &pscBuffer);
	if (pscBuffer)
	{
		pscBuffer->GetDesc(&pscdesc);
		pscBuffer->Release();
		pscBuffer = nullptr;
	}

	// get all the information we wanted, store them
	propertiesModel model{ stride,vedesc.ByteWidth,indesc.ByteWidth,pscdesc.ByteWidth };
	if (model == currentModel)
	{
		//pContext->PSSetShader(pShaderRed, nullptr, 0);
		for (int x1 = 0; x1 <= 10; x1++)
		{
			pContext->PSSetShaderResources(x1, 1, &textureView);
		}
	}
	if (Models.find(model) == Models.end())
	{
		mtx_models.lock();
		Models.insert(model);
		LOG_F(INFO, "Insert model info, total: %d", Models.size());
		mtx_models.unlock();
	}
	// iterate the set, operate current
	static auto current = Models.cbegin();
	// move forward
	if (GetAsyncKeyState(VK_F9) & 1)
	{
		currentModel = *current;
		if (current == Models.cend())
		{
			current = Models.cbegin();
			currentIndex = 0;
		}
		else
		{
			current++;
		}
		LOG_F(INFO, "Move Forward, now at:%d", currentIndex);
	}
	// move backward
	if (GetAsyncKeyState(VK_F8) & 1)
	{
		currentModel = *current;
		if (current == Models.cbegin())
		{
			current = Models.cend();
			currentIndex = Models.size();
		}
		else
		{
			current--;
			currentIndex--;
		}
		LOG_F(INFO, "Move Backward, now at:%d", currentIndex);
	}

	if (GetAsyncKeyState('L') & 1)
	{
		currentModel.print();
	}

	return oDrawIndexed(pContext, IndexCount, StartIndexLocation, BaseVertexLocation);
}

DWORD WINAPI MainThread(LPVOID lpReserved)
{
	// create console
	AllocConsole();
	FILE* f;
	freopen_s(&f, "CONOUT$", "w", stderr);

	// init log
	loguru::add_file("detail.log", loguru::Truncate, loguru::Verbosity_MAX);
	loguru::add_file("error.log", loguru::Truncate, loguru::Verbosity_WARNING);

	bool init_hook = false;
	do
	{
		if (kiero::init(kiero::RenderType::D3D11) == kiero::Status::Success)
		{
			kiero::bind(8, (void**)&oPresent, hkPresent);
			kiero::bind(73, (void**)&oDrawIndexed, hDrawIndexed);
			init_hook = true;
			LOG_F(INFO, "Hook created");
		}
	} while (!init_hook);
	return TRUE;
}

BOOL WINAPI DllMain(HMODULE hMod, DWORD dwReason, LPVOID lpReserved)
{
	switch (dwReason)
	{
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(hMod);
		CreateThread(nullptr, 0, MainThread, hMod, 0, nullptr);
		break;
	case DLL_PROCESS_DETACH:
		kiero::shutdown();
		break;
	}
	return TRUE;
}

HRESULT GenerateShader(ID3D11Device* pD3DDevice, ID3D11PixelShader** pShader, float r, float g, float b)
{
	char szCast[] = "struct VS_OUT"
		"{"
		"    float4 Position   : SV_Position;"
		"    float4 Color    : COLOR0;"
		"};"

		"float4 main( VS_OUT input ) : SV_Target"
		"{"
		"    float4 fake;"
		"    fake.a = 1.0;"
		"    fake.r = %f;"
		"    fake.g = %f;"
		"    fake.b = %f;"
		"    return fake;"
		"}";
	ID3D10Blob* pBlob;
	char szPixelShader[1000];

	//TODO Test the whether the function is OK
	sprintf_s(szPixelShader, sizeof(szPixelShader), szCast, r, g, b);

	HRESULT hr = D3DCompile(szPixelShader, sizeof(szPixelShader), "shader", NULL, NULL, "main", "ps_4_0", NULL, NULL, &pBlob, NULL);

	if (FAILED(hr))
		return hr;

	hr = pD3DDevice->CreatePixelShader((DWORD*)pBlob->GetBufferPointer(), pBlob->GetBufferSize(), NULL, pShader);

	if (FAILED(hr))
		return hr;

	return S_OK;
}