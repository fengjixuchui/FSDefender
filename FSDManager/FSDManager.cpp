// FSDManager.cpp : Defines the entry point for the console application.
//
#include "CFSDPortConnector.h"
#include "FSDCommonInclude.h"
#include "FSDCommonDefs.h"
#include "stdio.h"
#include "AutoPtr.h"
#include "FSDThreadUtils.h"
#include "Shlwapi.h"
#include <math.h>
#include <fstream>
#include <vector>
#include "LZJD.h"
#include "MurmurHash3.h"
#include "CFSDDynamicByteBuffer.h"
#include <unordered_map>
#include "FSDUmFileUtils.h"
#include <iostream>
#include "Psapi.h"
#include "FSDThreadUtils.h"

using namespace std;

HRESULT HrMain();

#define MAX_COMMAND_LENGTH 10
#define MAX_PARAMETER_LENGTH 256

#define FSD_INPUT_THREADS_COUNT 8
#define KB 1024
#define MB KB*KB
#define MAX_BUFFER_SIZE (2*MB)
#define LZJDISTANCE_THRESHOLD 40 // 0: two blobs of random data; 100: high likelihood that two files are related
#define ENTROPY_THRESHOLD 0.5
#define WRITE_ENTROPY_TRIGGER 7.9

uint64_t digest_size = 1024;

struct THREAD_CONTEXT
{
    bool               fExit;
    CFSDPortConnector* pConnector;
    CAutoStringW       wszScanDir;
};

static bool isFileFromSafeDir(wstring wszFileName, wstring wsdDirName)
{
    return wcsstr(wszFileName.c_str(), wsdDirName.c_str()) != NULL;
}

LPCWSTR MajorTypeToString(ULONG uMajorType)
{
    switch (uMajorType)
    {
    case IRP_CREATE:
        return L"IRP_CREATE";
    case IRP_CLOSE:
        return L"IRP_CLOSE";
    case IRP_READ:
        return L"IRP_READ";
    case IRP_WRITE:
        return L"IRP_WRITE";
    case IRP_QUERY_INFORMATION:
        return L"IRP_QUERY_INFORMATION";
    case IRP_SET_INFORMATION:
        return L"IRP_SET_INFORMATION";
    case IRP_CLEANUP:
        return L"IRP_CLEANUP";
    }

    return L"IRP_UNKNOWN";
}

int main(int argc, char **argv)
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    HRESULT hr = HrMain();
    if (FAILED(hr))
    {
        printf("Main failed with status 0x%x\n", hr);
        return 1;
    }

    return 0;
}

HRESULT ChangeDirectory(CFSDPortConnector* pConnector, THREAD_CONTEXT* pContext, LPCWSTR wszDirectory)
{
    HRESULT hr = S_OK;

    if (!PathFileExistsW(wszDirectory))
    {
        printf("Directory: %ls is not valid\n", wszDirectory);
        return S_OK;
    }

    CAutoStringW wszVolumePath = new WCHAR[50];
    hr = GetVolumePathNameW(wszDirectory, wszVolumePath.Get(), 50);
    RETURN_IF_FAILED(hr);

    size_t cVolumePath = wcslen(wszVolumePath.Get());

    FSD_MESSAGE_FORMAT aMessage;
    aMessage.aType = MESSAGE_TYPE_SET_SCAN_DIRECTORY;
    wcscpy_s(aMessage.wszFileName, MAX_FILE_NAME_LENGTH, wszDirectory + cVolumePath);

    printf("Changing directory to: %ls\n", wszDirectory);

    CAutoStringW wszScanDir;
    hr = NewCopyStringW(&wszScanDir, aMessage.wszFileName, MAX_FILE_NAME_LENGTH);
    RETURN_IF_FAILED(hr);

    wszScanDir.Detach(&pContext->wszScanDir);

    hr = pConnector->SendMessage((LPVOID)&aMessage, sizeof(aMessage), NULL, NULL);
    RETURN_IF_FAILED(hr);

    return S_OK;
}

HRESULT OnChangeDirectoryCmd(CFSDPortConnector* pConnector, THREAD_CONTEXT* pContext)
{
    HRESULT hr = S_OK;

    CAutoStringW wszParameter = new WCHAR[MAX_PARAMETER_LENGTH];
    RETURN_IF_FAILED_ALLOC(wszParameter);

    wscanf_s(L"%ls[/]", wszParameter.Get(), MAX_FILE_NAME_LENGTH);

    hr = ChangeDirectory(pConnector, pContext, wszParameter.Get());
    RETURN_IF_FAILED(hr);

    return S_OK;
}

static const char* szKillProcessLogo =
" -------------------------------------------------- \n"
"                                                    \n"
"                                                    \n"
"                 Process %u KILLED                  \n"
"                                                    \n"
"                                                    \n"
" -------------------------------------------------- \n";

HRESULT KillProcess(CFSDPortConnector* pConnector, ULONG uPid)
{
    UNREFERENCED_PARAMETER(pConnector);

    printf(szKillProcessLogo, uPid);

    CAutoHandle hProcess = OpenProcess(PROCESS_TERMINATE, false, uPid);
    if (!hProcess)
    {
        printf("Failed to open process %u\n", uPid);
        return S_FALSE;
    }

    bool fSuccess = TerminateProcess(hProcess, 0);
    if (!fSuccess)
    {
        printf("Failed to terminate process %u\n", uPid);
        return S_FALSE;
    }

    return S_OK;
}

HRESULT OnSendMessageCmd(CFSDPortConnector* pConnector)
{
    HRESULT hr = S_OK;

    CAutoStringW wszParameter = new WCHAR[MAX_PARAMETER_LENGTH];
    RETURN_IF_FAILED_ALLOC(wszParameter);

    wscanf_s(L"%ls", wszParameter.Get(), MAX_FILE_NAME_LENGTH);

    FSD_MESSAGE_FORMAT aMessage;
    aMessage.aType = MESSAGE_TYPE_PRINT_STRING;
    wcscpy_s(aMessage.wszFileName, MAX_FILE_NAME_LENGTH, wszParameter.Get());

    printf("Sending message: %ls\n", wszParameter.Get());

    BYTE pReply[MAX_STRING_LENGTH];
    DWORD dwReplySize = sizeof(pReply);
    hr = pConnector->SendMessage((LPVOID)&aMessage, sizeof(aMessage), pReply, &dwReplySize);
    RETURN_IF_FAILED(hr);

    if (dwReplySize > 0)
    {
        printf("Recieved response: %ls\n", (WCHAR*)pReply);
    }

    return S_OK;
}

class CFileExtention;
class CFileInformation;

class CFileExtention
{
public:
    CFileExtention() = default;

    CFileExtention(wstring wszExtention)
        : wszExtention(wszExtention)
        , cAccessedForRead(0)
        , cAccessedForWrite(0)
    {}

    void RegisterAccess(FSD_OPERATION_DESCRIPTION* pOperation)
    {
        if (pOperation->uMajorType == IRP_READ)
        {
            cAccessedForRead++;
        }

        if (pOperation->uMajorType == IRP_WRITE)
        {
            cAccessedForWrite++;
        }
    }

    size_t readAccess()
    {
        return cAccessedForRead;
    }

    size_t writeAccess()
    {
        return cAccessedForWrite;
    }

private:
    size_t  cAccessedForRead;
    size_t  cAccessedForWrite;

    wstring wszExtention;
};

class CProcess
{
public:
    CProcess(ULONG uPid)
        : uPid(uPid)
        , dSumOfWeightedReadEntropies(0)
        , dSumOfWeightsForReadEntropy(0)
        , dSumOfWeightedWriteEntropies(0)
        , dSumOfWeightsForWriteEntropy(0)
        , cFilesDeleted(0)
        , cFilesRenamed(0)
        , cbFilesRead(0)
        , cbFilesWrite(0)
        , cLZJDistanceExceed(0)
        , cFilesMovedFromUnsafeFolder(0)
        , cFilesMovedToUnsafeFolder(0)
        , cPrint(0)
        , cChangedExtensions(0)
    {
        GetProcessNameByPid(uPid, wszProcessName, MAX_FILE_NAME_LENGTH);
    }

    void addFileExstension(FSD_OPERATION_DESCRIPTION* pOperation)
    {
         LPCWSTR wszFileExtention = GetFileExtentionFromFileName(pOperation->GetFileName());
         if (!wszFileExtention)
         {
             return;
         }

         auto fileExtension = aFileExtentions.insert({ wszFileExtention, CFileExtention(wszFileExtention) });
         fileExtension.first->second.RegisterAccess(pOperation);
    }

    bool IsMalicious()
    {
        printInfo();
        
        ULONG uTrigger = 0;

        uTrigger += EntropyTrigger();
        uTrigger += FileDistanceTrigger();
        //uTrigger += FileExtentionsTrigger();
        uTrigger += DeletionTrigger();
        uTrigger += RenameTrigger();
        //uTrigger += AccessTypeTrigger();
        uTrigger += MoveToFolderTrigger();
        //uTrigger += RemoveFromFolderTrigger();
        uTrigger += ChangeExtensionTrigger();

        return uTrigger >= 3;
    }

    void deleteFile()
    {
        cFilesDeleted++;
    }

    void LZJDistanceExceed()
    {
        cLZJDistanceExceed++;
    }

    void renameFile()
    {
        cFilesRenamed++;
    }

    void moveFileFromUnsafeFolder()
    {
        cFilesMovedFromUnsafeFolder++;
    }

    void moveFileToSafeZone()
    {
        cFilesMovedToUnsafeFolder++;
    }

    ULONG getPid()
    {
        return uPid;
    }

    void updateWriteEntropy(double dWriteEntropy, size_t cbWrite)
    {
        double w = 0.125 * round(dWriteEntropy) * cbWrite;
        dSumOfWeightedWriteEntropies += w * dWriteEntropy;
        dSumOfWeightsForWriteEntropy += w;

        cbFilesWrite += cbWrite;
    }

    void updateReadEntropy(double dReadEntropy, size_t cbRead)
    {
        double w = 0.125 * round(dReadEntropy) * cbRead;
        dSumOfWeightedReadEntropies += w * dReadEntropy;
        dSumOfWeightsForReadEntropy += w;

        cbFilesRead += cbRead;
    }

    void changeExtension()
    {
        cChangedExtensions++;
    }

    void printInfo(bool fUnconditionally = false)
    {
        if (cPrint % 1000 == 0 || fUnconditionally)
        {
            printf("Process: %ls PID: %u\n", wszProcessName, uPid);
            cout << "Read: " << cbFilesRead << " Bytes, Write: " << cbFilesWrite << " Bytes" << endl
                << "Files Deleted: " << cFilesDeleted << endl
                << "Files Renamed: " << cFilesRenamed << endl
                << "Write entropy: " << ((dSumOfWeightedWriteEntropies > 0) ? dSumOfWeightedWriteEntropies / dSumOfWeightsForWriteEntropy : 0) << endl
                << "Read entropy: " << ((dSumOfWeightedReadEntropies > 0) ? dSumOfWeightedReadEntropies / dSumOfWeightsForReadEntropy : 0) << endl
                << "Removed From folder: " << cFilesMovedFromUnsafeFolder << endl
                << "Moved to folder: " << cFilesMovedToUnsafeFolder << endl
                << "LZJ distance exceeded: " << cLZJDistanceExceed << endl
                << "File extensions changed: " << cChangedExtensions << endl;
            printf("----------------------------------------------------------------------\n");
        }

        cPrint++;
    }


private:
    bool EntropyTrigger()
    {
        if (dSumOfWeightedWriteEntropies > 0 && dSumOfWeightedReadEntropies > 0)
        {
            return dSumOfWeightedWriteEntropies / dSumOfWeightsForWriteEntropy -
                dSumOfWeightedReadEntropies / dSumOfWeightsForReadEntropy > ENTROPY_THRESHOLD;
        }
        else if (dSumOfWeightedWriteEntropies > 0)
        {
            return dSumOfWeightedWriteEntropies / dSumOfWeightsForWriteEntropy > WRITE_ENTROPY_TRIGGER;
        }

        return false;
    }

    bool FileDistanceTrigger()
    {
        return cLZJDistanceExceed > 0;
    }

    bool RenameTrigger()
    {
        return cFilesRenamed > 0;
    }
    bool  DeletionTrigger()
    {
        return cFilesDeleted + cFilesMovedToUnsafeFolder > 0;
    }

    bool RemoveFromFolderTrigger()
    {
        return cFilesMovedToUnsafeFolder > 0;
    }

    bool MoveToFolderTrigger()
    {
        return cFilesMovedFromUnsafeFolder > 0;
    }

    bool ChangeExtensionTrigger()
    {
        return cChangedExtensions > 0;
    }

    ULONG uPid;

    WCHAR wszProcessName[MAX_FILE_NAME_LENGTH];

    size_t cPrint;

    double dSumOfWeightedWriteEntropies;
    double dSumOfWeightedReadEntropies;

    double dSumOfWeightsForWriteEntropy;
    double dSumOfWeightsForReadEntropy;

    size_t cFilesDeleted;
    size_t cFilesRenamed;
    size_t cbFilesRead;
    size_t cbFilesWrite;
    size_t cLZJDistanceExceed;
    size_t cFilesMovedFromUnsafeFolder;
    size_t cFilesMovedToUnsafeFolder;
    size_t cChangedExtensions;

    unordered_map<wstring, CFileExtention> aFileExtentions;
};

class CFileInformation
{
public:
    CFileInformation(LPCWSTR wszFileName)
        : wszFileName(wszFileName)
        , cAccessedForRead(0)
        , cAccessedForWrite(0)
        , fOpened(true)
        , fCheckForDelete(false)
        , fRecalculateSimilarity(true)
        , fMovedFromSafeZone(false)
        , dAverageWriteEntropy(0)
        , dAverageReadEntropy(0)
    {}

    void RegisterAccess(FSD_OPERATION_DESCRIPTION* pOperation, CProcess* pProcess);
    void updateFileName(wstring newName)
    {
        wszFileName = newName;
    }
    void movedFromSafeZone()
    {
        fMovedFromSafeZone = true;
    }

public:
    wstring wszFileName;
    size_t cAccessedForRead;
    size_t cAccessedForWrite;

    double dAverageWriteEntropy;
    double dAverageReadEntropy;
    ULONG  uLastCalculatedZLJ;

    bool fOpened;
    bool fCheckForDelete;
    bool fRecalculateSimilarity;
    bool fMovedFromSafeZone;
    unordered_map<ULONG, CProcess*> aProcesses;
    vector<int> LZJvalue;
};

unordered_map<wstring, CFileInformation> gFiles;
unordered_map<ULONG, CProcess> gProcesses;

void CFileInformation::RegisterAccess(FSD_OPERATION_DESCRIPTION* pOperation, CProcess* pProcess)
{
    // add exstension to process
    pProcess->addFileExstension(pOperation);
    // add process to hash
    aProcesses.insert({ pProcess->getPid() , pProcess });

    switch (pOperation->uMajorType)
    {
        case IRP_READ:
        {
            cAccessedForRead++;

            FSD_OPERATION_READ* pReadOp = pOperation->ReadDescription();

            if (pReadOp->fReadEntropyCalculated)
            {
                dAverageWriteEntropy = pReadOp->dReadEntropy;
                pProcess->updateReadEntropy(pReadOp->dReadEntropy, pReadOp->cbRead);
                //printf("Read size: %Iu Bytes, Read entropy: %0.5f\n", pReadOp->cbRead, pReadOp->dReadEntropy);
            }

            break;
        }

        case IRP_WRITE:
        {
            cAccessedForWrite++;

            FSD_OPERATION_WRITE* pWriteOp = pOperation->WriteDescription();

            if (pWriteOp->fWriteEntropyCalculated)
            {
                dAverageWriteEntropy = pWriteOp->dWriteEntropy;
                pProcess->updateWriteEntropy(pWriteOp->dWriteEntropy, pWriteOp->cbWrite);
                //printf("Write entropy: %0.5f\n", dAverageWriteEntropy);
                fRecalculateSimilarity = true;
            }

            break;
        }

        case IRP_CREATE:
        {
            fCheckForDelete = pOperation->fCheckForDelete;
            // LZJvalue already computed
            if (!LZJvalue.empty())
            {
                break;
            }
            // calculate file initial LZJ
            CAutoHandle hFile;
            HRESULT hr = UtilTryToOpenFileW(&hFile, wszFileName.c_str(), 10);
            if (hr == E_FILE_NOT_FOUND)
            {
                fCheckForDelete = true;
                break;
            }
            VOID_IF_FAILED_EX(hr);

            CAutoArrayPtr<BYTE> pBuffer = new BYTE[2048];
            VOID_IF_FAILED_ALLOC(pBuffer);

            DWORD dwRead = 2048;
            hr = UtilReadFile(hFile, pBuffer.Get(), &dwRead);
            VOID_IF_FAILED(hr);

            LZJvalue = digest(digest_size, pBuffer.Get(), dwRead);
            break;
        }
        case IRP_CLEANUP:
        case IRP_CLOSE:
        {
            if (fCheckForDelete)
            {
                HRESULT hr = S_OK;

                CAutoHandle hFile;
                hr = UtilTryToOpenFileW(&hFile, wszFileName.c_str(), 10);
                if (hr == E_FILE_NOT_FOUND)
                {
                    fCheckForDelete = false;
                    pProcess->deleteFile();
                    break;
                }
                VOID_IF_FAILED_EX(hr);
            }

            if (!LZJvalue.empty() && fRecalculateSimilarity)
            {
                // calculate file final ZLJ and ZLJDistance
                CAutoHandle hFile;
                HRESULT hr = UtilTryToOpenFileW(&hFile, wszFileName.c_str(), 10);
                if (hr == E_FILE_NOT_FOUND)
                {
                    fCheckForDelete = false;
                    pProcess->deleteFile();
                }
                VOID_IF_FAILED_EX(hr);

                CAutoArrayPtr<BYTE> pBuffer = new BYTE[2048];
                VOID_IF_FAILED_ALLOC(pBuffer);

                DWORD dwRead = 2048;
                hr = UtilReadFile(hFile, pBuffer.Get(), &dwRead);
                VOID_IF_FAILED(hr);

                vector<int> LZJnewVaue = digest(digest_size, pBuffer.Get(), dwRead);

                ULONG uSimilarity = similarity(LZJvalue, LZJnewVaue);
                if (uSimilarity < LZJDISTANCE_THRESHOLD) // TODO: modify threshold
                {
                    printf("Similarity of %ls: %u\n", wszFileName.c_str(), uSimilarity);
                    pProcess->LZJDistanceExceed();
                }
                // update LZJ value
                LZJvalue = LZJnewVaue;
                fRecalculateSimilarity = false;
            }

            break;
        }

        case IRP_SET_INFORMATION:
        {
            fCheckForDelete = pOperation->fCheckForDelete;
            break;
        }

        default:
        {
            ASSERT(false);
        }
    }
}

void ProcessIrp(FSD_OPERATION_DESCRIPTION* pOperation, THREAD_CONTEXT* pContext)
{
    //printf("PID: %u MJ: %ls MI: %u\n", pOperation->uPid, MajorTypeToString(pOperation->uMajorType), pOperation->uMinorType);
    
    // add process to global hash
    auto process = gProcesses.insert({ pOperation->uPid , CProcess(pOperation->uPid) });
    CProcess* pProcess = &process.first->second;

    if (pOperation->uMajorType == IRP_SET_INFORMATION  && !pOperation->fCheckForDelete)
    {
        FSD_OPERATION_SET_INFORMATION* pSetInformation = pOperation->SetInformationDescription();
        bool oldFileFromSafeZone = isFileFromSafeDir(pSetInformation->GetInitialFileName(), pContext->wszScanDir.Get());
        bool newFileFromSafeZone = isFileFromSafeDir(pSetInformation->GetNewFileName(), pContext->wszScanDir.Get());

        auto oldFile = gFiles.find(pSetInformation->GetInitialFileName());
        if (oldFile == gFiles.end())   // new file copied to our folder
        {
            ASSERT(newFileFromSafeZone == TRUE);
            auto file = gFiles.insert({ pSetInformation->GetNewFileName(), CFileInformation(pSetInformation->GetNewFileName()) });
            file.first->second.RegisterAccess(pOperation, pProcess);
        }
        else
        {
            LPCWSTR wszFileExtention = GetFileExtentionFromFileName(pSetInformation->GetInitialFileName());
            LPCWSTR wszNewFileExtension = GetFileExtentionFromFileName(pSetInformation->GetNewFileName());
            if (wszFileExtention || wszNewFileExtension)
            {
                pProcess->changeExtension();
            }
            if (oldFileFromSafeZone)
            {
                CFileInformation newFileInfo(oldFile->second);

                if (newFileFromSafeZone)            
                {
                    pProcess->renameFile();     // rename file in safe zone
                }
                else
                {
                    pProcess->moveFileFromUnsafeFolder();       // or move file from safe zone
                    newFileInfo.movedFromSafeZone();
                }

                newFileInfo.updateFileName(pSetInformation->GetNewFileName());
                gFiles.erase(oldFile);
                auto file = gFiles.insert({ pSetInformation->GetNewFileName(), newFileInfo });
                file.first->second.RegisterAccess(pOperation, pProcess);
            }
            else if (newFileFromSafeZone)
            {
                ASSERT(FALSE);      // this case processed above (new file copied to our folder)
            }
            else
            {
                ASSERT(FALSE);      // rename file out of safe zone
            }
        }
    }
    else
    {
        auto file = gFiles.insert({ pOperation->GetFileName(), CFileInformation(pOperation->GetFileName()) });
        file.first->second.RegisterAccess(pOperation, pProcess);
    }

    if (pProcess->IsMalicious())
    {
        pProcess->printInfo(true);
        HRESULT hr = KillProcess(pContext->pConnector, pProcess->getPid());
        VOID_IF_FAILED(hr);
        gProcesses.erase(pProcess->getPid());
    }
}

HRESULT FSDIrpSniffer(PVOID pvContext)
{
    HRESULT hr = S_OK;

    THREAD_CONTEXT* pContext = static_cast<THREAD_CONTEXT*>(pvContext);
    RETURN_IF_FAILED_ALLOC(pContext);

    CFSDPortConnector* pConnector = pContext->pConnector;
    ASSERT(pConnector != NULL);

    CFSDDynamicByteBuffer pBuffer;
    hr = pBuffer.Initialize(1024*8);
    RETURN_IF_FAILED(hr);

    size_t cTotalIrpsRecieved = 0;
    while (!pContext->fExit)
    {
        FSD_MESSAGE_FORMAT aMessage;
        aMessage.aType = MESSAGE_TYPE_QUERY_NEW_OPS;

        BYTE* pResponse = pBuffer.Get();
        DWORD dwReplySize = numeric_cast<DWORD>(pBuffer.ReservedSize());
        hr = pConnector->SendMessage((LPVOID)&aMessage, sizeof(aMessage), pBuffer.Get(), &dwReplySize);
        RETURN_IF_FAILED(hr);

        if (dwReplySize == 0)
        {
            continue;
        }

        FSD_OPERATION_DESCRIPTION* pOpDescription = ((FSD_QUERY_NEW_OPS_RESPONSE_FORMAT*)(PVOID)pResponse)->GetFirst();
        size_t cbData = 0;
        size_t cCurrentIrpsRecieved = 0;
        for (;;)
        {
            if (cbData >= dwReplySize)
            {
                ASSERT(cbData == dwReplySize);
                break;
            }

            try
            {
                ProcessIrp(pOpDescription, pContext);
            }
            catch (...)
            {
                printf("Exception in ProcessIrp!!!\n");
                return S_OK;
            }

            cbData += pOpDescription->PureSize();
            cCurrentIrpsRecieved++;
            pOpDescription = pOpDescription->GetNext();
        }

        cTotalIrpsRecieved += cCurrentIrpsRecieved;

        printf("Total IRPs: %Iu Current Irps: %Iu Recieve size: %Iu Buffer size: %Iu Buffer utilization: %.2lf%%\n", 
            cTotalIrpsRecieved, cCurrentIrpsRecieved, cbData, pBuffer.ReservedSize(), ((double)cbData / pBuffer.ReservedSize() ) * 100);

        if (pBuffer.ReservedSize() < MAX_BUFFER_SIZE && cbData >= pBuffer.ReservedSize()*2/3)
        {
            pBuffer.Grow();
        }

        if (cbData < pBuffer.ReservedSize()/2)
        {
            Sleep(1000);
        }
    }

    return S_OK;
}

HRESULT UserInputParser(PVOID pvContext)
{
    HRESULT hr = S_OK;

    THREAD_CONTEXT* pContext = static_cast<THREAD_CONTEXT*>(pvContext);
    RETURN_IF_FAILED_ALLOC(pContext);

    CFSDPortConnector* pConnector = pContext->pConnector;
    ASSERT(pConnector != NULL);
    
    hr = ChangeDirectory(pConnector, pContext, L"C:\\Users\\User\\");
    RETURN_IF_FAILED(hr);

    CAutoStringW wszCommand = new WCHAR[MAX_COMMAND_LENGTH];
    RETURN_IF_FAILED_ALLOC(wszCommand);

    while (!pContext->fExit)
    {
        printf("Input a command: ");
        wscanf_s(L"%ls", wszCommand.Get(), MAX_COMMAND_LENGTH);
        if (wcscmp(wszCommand.Get(), L"chdir") == 0)
        {
            hr = OnChangeDirectoryCmd(pConnector, pContext);
            RETURN_IF_FAILED(hr);
        } 
        else
        if (wcscmp(wszCommand.Get(), L"message") == 0)
        {
            hr = OnSendMessageCmd(pConnector);
            RETURN_IF_FAILED(hr);
        }
        else
        if (wcscmp(wszCommand.Get(), L"exit") == 0)
        {
            pContext->fExit = true;
            printf("Exiting FSDManager\n");
        }
        else
        if (wcscmp(wszCommand.Get(), L"kill") == 0)
        {
            ULONG uPid;
            if (wscanf_s(L"%u", &uPid))
            {
                printf("Killing process %u FSDManager\n", uPid);
                hr = KillProcess(pConnector, uPid);
                RETURN_IF_FAILED_EX(hr);
            }
            else
            {
                printf("Failed to read PID\n");
            }
        }
        else
        {
            printf("Invalid command: %ls\n", wszCommand.Get());
        }
    }

    return S_OK;
}

HRESULT HrMain()
{
    HRESULT hr = S_OK;

    CAutoPtr<CFSDPortConnector> pConnector;
    hr = NewInstanceOf<CFSDPortConnector>(&pConnector, g_wszFSDPortName);
    if (hr == E_FILE_NOT_FOUND)
    {
        printf("Failed to connect to FSDefender Kernel module. Try to load it.\n");
    }
    RETURN_IF_FAILED(hr);

    FSD_MESSAGE_FORMAT aMessage;
    aMessage.aType = MESSAGE_TYPE_SET_MANAGER_PID;
    aMessage.uPid = GetCurrentProcessId();

    hr = pConnector->SendMessage((LPVOID)&aMessage, sizeof(aMessage), NULL, NULL);
    RETURN_IF_FAILED(hr);

    THREAD_CONTEXT aContext = {};
    aContext.fExit           = false;
    aContext.pConnector      = pConnector.Get();

    CAutoHandle hFSDIrpSnifferThread;
    hr = UtilCreateThreadSimple(&hFSDIrpSnifferThread, (LPTHREAD_START_ROUTINE)FSDIrpSniffer, (PVOID)&aContext);
    RETURN_IF_FAILED(hr);
    
    CAutoHandle hUserInputParserThread;
    hr = UtilCreateThreadSimple(&hUserInputParserThread, (LPTHREAD_START_ROUTINE)UserInputParser, (PVOID)&aContext);
    RETURN_IF_FAILED(hr);

    hr = WaitForSingleObject(hFSDIrpSnifferThread.Get(), INFINITE);
    RETURN_IF_FAILED(hr);

    hr = WaitForSingleObject(hUserInputParserThread.Get(), INFINITE);
    RETURN_IF_FAILED(hr);

    return S_OK;
}