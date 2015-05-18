#include "stdafx.h"
#include "HelperCommandExecute.h"

#include "TCHAR.h"
#include "stdio.h"

#include <Tlhelp32.h>

#include "Toolhelp32Snapshot.h"


HelperCommandExecute::ConsumerList   HelperCommandExecute::_consumers;
HelperCommandExecute::SubscriberList HelperCommandExecute::_subscribers;

ThreadMutex HelperCommandExecute::_subscribersLock;
ThreadMutex HelperCommandExecute::_consumersLock;
ThreadMutex HelperCommandExecute::_dllLock;

HMODULE HelperCommandExecute::_dllHandle = NULL;
HWND HelperCommandExecute::_targetWindowHandle = NULL;
HINSTANCE HelperCommandExecute::_appHInstance = NULL;

BOOL HelperCommandExecute::_hooksInstalled = FALSE;


void HelperCommandExecute::Initialize(HINSTANCE hInstance)
{
	_dllHandle = NULL;
	_targetWindowHandle = NULL;
	_appHInstance = hInstance;
}


void HelperCommandExecute::SetTargetWindow(HWND tiHelperWindowHandle)
{
	_targetWindowHandle = tiHelperWindowHandle;

	if (!_hooksInstalled && GetSubscribersCount() > 0)
		InstallHooks(_targetWindowHandle);
}



void HelperCommandExecute::Uninitialize()
{
	RemoveHooks(_targetWindowHandle);
}


void HelperCommandExecute::BroadcastMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
	Lock l(_subscribersLock);
	SubscriberListIterator it(&_subscribers);
	while (it.next())
	{
		if (it.data() != NULL)
			::PostMessage(*it.data(), message, wParam, lParam);
	}
}

BOOL CALLBACK EnumThreadWndProc_BlockCheckPlugin(HWND hwnd, LPARAM lParam)
{
	Messenger().Post(hwnd, MessageNotifications::X19_CORE_BLOCK_CHECK_PLUGIN);

	return TRUE;
}


void HelperCommandExecute::BroadcastConsumers_BlockCheckPlugin()
{
	Lock l(_consumersLock);
	ConsumerListIterator it(&_consumers);
	while (it.next())
	{
		if (it.data() != NULL)
		{ 
			THREADENTRY32 te32;


			Toolhelp32Snapshot threadsSnapshot(TH32CS_SNAPTHREAD, 0 );

			if( (HANDLE)threadsSnapshot == INVALID_HANDLE_VALUE )
				continue;

			te32.dwSize = sizeof(THREADENTRY32 );

			if( !::Thread32First( (HANDLE)threadsSnapshot, &te32 ) )
				continue;

			// цикл обхода окон потока
			do
			{
				if( te32.th32OwnerProcessID == *it.data())
					EnumThreadWindows(te32.th32ThreadID, (WNDENUMPROC)EnumThreadWndProc_BlockCheckPlugin, NULL);

			} while( Thread32Next((HANDLE)threadsSnapshot, &te32 ) );
		}
	}

}


void HelperCommandExecute::AddConsumer(DWORD dwProcessId)
{
	Lock l(_consumersLock);
	_consumers.queue(new DWORD(dwProcessId));
}



void HelperCommandExecute::SubscribeWindow(HWND windowHandle)
{
	Lock l(_subscribersLock);
	if (!_hooksInstalled && _targetWindowHandle != NULL)
		InstallHooks(_targetWindowHandle);

	_subscribers.queue(new HWND(windowHandle));
}


void HelperCommandExecute::UnsubscribeWindow(HWND windowHandle)
{
	Lock l(_subscribersLock);
	SubscriberListIterator it(&_subscribers);
	it.next();

	SubscriberNode* currentNode = NULL;

	while (it.current())
	{
		currentNode = it.current();
		it.next();

		if (currentNode->Data() != NULL && *currentNode->Data() == windowHandle)
			_subscribers.exclude(currentNode);
	}

	if (_subscribers.count() == 0)
		RemoveHooks(_targetWindowHandle);
}


void HelperCommandExecute::CheckConsumersList()
{
	Lock l(_consumersLock);
	ConsumerListIterator it(&_consumers);
	it.next();

	ConsumerNode* currentNode = NULL;

	while (it.current())
	{
		currentNode = it.current();
		it.next();

		if (currentNode->Data() != NULL && _FindProcess(*currentNode->Data()) != *currentNode->Data())
			_consumers.exclude(currentNode);
	}
}


void HelperCommandExecute::CheckSubscribersList()
{
	Lock l(_subscribersLock);
	SubscriberListIterator it(&_subscribers);
	it.next();

	SubscriberNode* currentNode = NULL;

	while (it.current())
	{
		currentNode = it.current();
		it.next();

		if (currentNode->Data() != NULL && !::IsWindow(*currentNode->Data()))
			_subscribers.exclude(currentNode);
	}


	if (_subscribers.count() == 0)
		RemoveHooks(_targetWindowHandle);
}


INT32 HelperCommandExecute::GetConsumersCount()
{
	Lock l(_consumersLock);
	return _consumers.count();
}


DWORD HelperCommandExecute::GetConsumer(int index)
{
	Lock l(_consumersLock);
	return GetIndexedElement<DWORD>(&_consumers, index);
}


INT32 HelperCommandExecute::GetSubscribersCount()
{
	Lock l(_subscribersLock);
	return _subscribers.count();
}


HWND HelperCommandExecute::GetSubscriber(int index)
{
	Lock l(_subscribersLock);
	return GetIndexedElement<HWND>(&_subscribers, index);
}


template<class T>
T HelperCommandExecute::GetIndexedElement(List<T>* obj, int index)
{
	list_iterator<T> it(obj);
	int currentIndex = 0;
	while (it.next())
	{
		if (currentIndex == index)
		{
			if (it.data() != NULL)
				return *it.data();
			else
				return 0;
		}

		currentIndex++;
	}

	return NULL;
}


DWORD HelperCommandExecute::_FindProcess(DWORD dwProcessId)
{
	PROCESSENTRY32 processData;
	processData.dwSize = sizeof(processData);

	Toolhelp32Snapshot processesSnapshot(TH32CS_SNAPPROCESS);
	for (BOOL Ok = ::Process32First((HANDLE)processesSnapshot, &processData); Ok == TRUE; 
		Ok = ::Process32Next((HANDLE)processesSnapshot,&processData))
	{
		if (processData.th32ProcessID == dwProcessId)
			return dwProcessId;
	};
	return -1;
}


void HelperCommandExecute::GetControlDllFile(LPWSTR dllFilePath, int bufferSize)
{
	WCHAR currentProcessFileName[MAX_PATH];
	::ZeroMemory(currentProcessFileName, MAX_PATH * sizeof(WCHAR));
	::GetModuleFileNameW(NULL, currentProcessFileName, MAX_PATH);

	WCHAR drive[MAX_PATH] = L"";
	WCHAR dir[MAX_PATH] = L"";
	WCHAR selfName[MAX_PATH] = L"";
	WCHAR ext[MAX_PATH] = L"";
	WCHAR workingDirectory[MAX_PATH] = L"";
	WCHAR applicationPath[MAX_PATH] = L"";

	_wsplitpath_s(currentProcessFileName, drive, MAX_PATH, dir, MAX_PATH,
		selfName, MAX_PATH, ext, MAX_PATH);

	WCHAR dllSelfName[MAX_PATH] = L"";
	WCHAR* lpHelperTokenStarted = wcsstr(selfName, L"Helper");

	DWORD dwBeforeTokenHelperPartSize = lpHelperTokenStarted - selfName;
	wcsncpy_s(dllSelfName, MAX_PATH, selfName, dwBeforeTokenHelperPartSize);
	wcscat_s(dllSelfName, MAX_PATH, L"Control");

	DWORD dwSkippedPartSize = dwBeforeTokenHelperPartSize + wcslen(L"Helper");
	DWORD dwSelfNameSize = wcslen(selfName);

	wcsncat_s(dllSelfName, MAX_PATH, lpHelperTokenStarted + wcslen(L"Helper"), dwSelfNameSize - dwSkippedPartSize);

	_wmakepath_s(dllFilePath, bufferSize, drive, dir, dllSelfName, L".dll");
}


bool HelperCommandExecute::InstallHooks(HWND ownerWindowHandle)
{
	bool Ret = false;
	try
	{
		LoadDll();

		WCHAR controlDllFile[MAX_PATH] = L"";
		GetControlDllFile(controlDllFile, MAX_PATH);

		DynamicFn<Win_Hooks_Install_proto> Win_Hooks_Install(controlDllFile, "Win_Hooks_Install");

		if (Win_Hooks_Install.isValid())
			Ret = (TRUE==(*Win_Hooks_Install)((DWORD)ownerWindowHandle));

		_hooksInstalled = TRUE;
	}
	catch (...) {}
	return Ret;
}


void HelperCommandExecute::LoadDll()
{
	try
	{
		Lock l(_dllLock);
		if (_dllHandle == NULL)
		{
			WCHAR dllFilePath[MAX_PATH];
			GetControlDllFile(dllFilePath, MAX_PATH);

			_dllHandle = ::LoadLibraryW(dllFilePath);
		}
	}
	catch(...)
	{}
}


void HelperCommandExecute::UnloadDll()
{
	try
	{
		Lock l(_dllLock);
		if (_dllHandle != NULL)
		{
			::FreeLibrary(_dllHandle);
			_dllHandle = NULL;
		}
	}
	catch(...)
	{}
}


bool HelperCommandExecute::RemoveHooks(HWND ownerWindowHandle)
{
	bool Ret=false;
	try
	{
		if (_dllHandle == NULL)
			return false;

		WCHAR controlDllFile[MAX_PATH] = L"";
		GetControlDllFile(controlDllFile, MAX_PATH);

		DynamicFn<Win_Hooks_Remove_proto> Win_Hooks_Remove(controlDllFile, "Win_Hooks_Remove");

		if (Win_Hooks_Remove.isValid())
			Ret=(TRUE==(*Win_Hooks_Remove)((DWORD)ownerWindowHandle));

		UnloadDll();

		_hooksInstalled = FALSE;
	}
	catch(...) {}

	return Ret;
}


void HelperCommandExecute::AdapterIsSlow(BOOL isSlow)
{
	try
	{
		if (_dllHandle == NULL)
			return;

		WCHAR controlDllFile[MAX_PATH] = L"";
		GetControlDllFile(controlDllFile, MAX_PATH);

		DynamicFn<Win_Hooks_AdapterIsSlow_proto> Win_Hooks_AdapterIsSlow(controlDllFile, "Win_Hooks_AdapterIsSlow");

		if (Win_Hooks_AdapterIsSlow.isValid())
			(*Win_Hooks_AdapterIsSlow)((BOOL)isSlow);
	}
	catch(...) {}
}
