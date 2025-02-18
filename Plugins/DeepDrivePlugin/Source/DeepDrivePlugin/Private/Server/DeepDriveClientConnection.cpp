// Fill out your copyright notice in the Description page of Project Settings.

#include "DeepDrivePluginPrivatePCH.h"
#include "DeepDriveClientConnection.h"

#include "Runtime/Sockets/Public/Sockets.h"
#include "Public/Server/Messages/DeepDriveServerMessageHeader.h"
#include "Public/Server/Messages/DeepDriveServerConnectionMessages.h"
#include "Public/Server/Messages/DeepDriveServerConfigurationMessages.h"

#include "Public/CaptureSink/SharedMemSink/SharedMemCaptureSinkComponent.h"
#include "Private/Server/DeepDriveServer.h"

#include "Private/Capture/DeepDriveCapture.h"

using namespace deepdrive::server;

DEFINE_LOG_CATEGORY(LogDeepDriveClientConnection);

DeepDriveClientConnection::DeepDriveClientConnection(FSocket *socket)
	:	m_Socket(socket)
	,	m_ClientId()
	,	m_isStopped(false)
	,	m_ReceiveBuffer(0)
	,	m_curReceiveBufferSize(0)

{
	(void)resizeReceiveBuffer(64 * 1024);

	m_WorkerThread = FRunnableThread::Create(this, TEXT("DeepDriveClientConnection"), 0, TPri_Normal);
}

DeepDriveClientConnection::~DeepDriveClientConnection()
{
	UE_LOG(LogDeepDriveClientConnection, Log, TEXT("[%d] DeepDriveClientConnection::~DeepDriveClientConnection"), m_ClientId);
}


bool DeepDriveClientConnection::Init()
{
	m_MessageHandlers[deepdrive::server::MessageId::RegisterClientRequest] = std::bind(&DeepDriveClientConnection::registerClient, this, std::placeholders::_1, std::placeholders::_2);
	m_MessageHandlers[deepdrive::server::MessageId::UnregisterClientRequest] = std::bind(&DeepDriveClientConnection::unregisterClient, this, std::placeholders::_1, std::placeholders::_2);

	std::function<void(const deepdrive::server::MessageHeader&, bool)> forward2Server = [](const deepdrive::server::MessageHeader &message, bool isMaster) { if (isMaster) DeepDriveServer::GetInstance().enqueueMessage(message.clone()); };
	m_MessageHandlers[deepdrive::server::MessageId::RegisterCaptureCameraRequest] = forward2Server;
	m_MessageHandlers[deepdrive::server::MessageId::RequestAgentControlRequest] = forward2Server;
	m_MessageHandlers[deepdrive::server::MessageId::ReleaseAgentControlRequest] = forward2Server;
	m_MessageHandlers[deepdrive::server::MessageId::SetAgentControlValuesRequest] = forward2Server;
	m_MessageHandlers[deepdrive::server::MessageId::ResetAgentRequest] = forward2Server;


	m_MessageAssembler.m_HandleMessage.BindRaw(this, &DeepDriveClientConnection::handleClientRequest);
	return m_Socket != 0;
}

uint32 DeepDriveClientConnection::Run()
{
	while (!m_isStopped)
	{
		float sleepTime = 0.01f;
		uint32 pendingSize = 0;
		if (m_Socket->HasPendingData(pendingSize))
		{
			if (pendingSize)
			{
				if (resizeReceiveBuffer(pendingSize))
				{
					int32 bytesRead = 0;
					if (m_Socket->Recv(m_ReceiveBuffer, m_curReceiveBufferSize, bytesRead, ESocketReceiveFlags::None))
					{
						// UE_LOG(LogDeepDriveClientConnection, Log, TEXT("[%d] Received %d bytes: %d"), m_ClientId, bytesRead, bytesRead > 4 ? *(reinterpret_cast<uint32*>(m_ReceiveBuffer)) : 0);
						m_MessageAssembler.add(m_ReceiveBuffer, bytesRead);
						sleepTime = 0.0f;
					}
				}

			}
		}

		deepdrive::server::MessageHeader *response = 0;
		if	(	m_ResponseQueue.Dequeue(response)
			&&	response
			)
		{
			int32 bytesSent = 0;
			m_Socket->Send(reinterpret_cast<uint8*> (response), response->message_size, bytesSent);
			// UE_LOG(LogDeepDriveClientConnection, Log, TEXT("[%d] %d bytes sent back"), m_ClientId, bytesSent);
			
			FMemory::Free(response);
			sleepTime = 0.0f;
		}

		if	(	m_isStopped == false
			&&	sleepTime > 0.0f
			)
			FPlatformProcess::Sleep(sleepTime);

	}

	shutdown();
	return 0;
}

void DeepDriveClientConnection::Stop()
{
	m_isStopped = true;
}

void DeepDriveClientConnection::Exit()
{
	delete this;
}

void DeepDriveClientConnection::handleClientRequest(const deepdrive::server::MessageHeader &message)
{
	MessageHandlers::iterator fIt = m_MessageHandlers.find(message.message_id);
	if (fIt != m_MessageHandlers.end())
		fIt->second(message, m_isMaster);
	else
		UE_LOG(LogDeepDriveClientConnection, Log, TEXT("[%d] Unknown message received type %d size %d"), m_ClientId, static_cast<uint32> (message.message_id), message.message_size);

}

void DeepDriveClientConnection::registerClient(const deepdrive::server::MessageHeader &message, bool isMaster)
{
	const RegisterClientRequest &regClient = static_cast<const RegisterClientRequest &> (message);

	m_isMaster = regClient.request_master_role > 0 ? true : false;
	m_ClientId = DeepDriveServer::GetInstance().registerClient(this, m_isMaster);

	UE_LOG(LogDeepDriveClientConnection, Log, TEXT("[%d] Client wants to register reqMaster %c isMaster %c"), m_ClientId, regClient.request_master_role ? 'T' : 'F', m_isMaster ? 'T' : 'F');

	RegisterClientResponse response;
	response.client_id = m_ClientId;
	response.granted_master_role = m_isMaster ? 1 : 0;

	const FString contentPath = FPaths::ConvertRelativePathToFull(FPaths::GameContentDir());
	const FString versionPath = FPaths::Combine(contentPath, "Data", "VERSION");
	FString buildTimeStamp;
	FFileHelper::LoadFileToString(buildTimeStamp, *versionPath);

	strncpy(response.server_protocol_version, TCHAR_TO_ANSI(*buildTimeStamp), RegisterClientResponse::ServerProtocolStringSize - 1);

	USharedMemCaptureSinkComponent *sharedMemSink = DeepDriveCapture::GetInstance().getSharedMemorySink();
	if (sharedMemSink)
	{
		const FString &sharedMemName = sharedMemSink->getSharedMemoryName();
		strncpy(response.shared_memory_name, TCHAR_TO_ANSI(*sharedMemName), RegisterClientResponse::SharedMemNameSize - 1);
		response.shared_memory_name[RegisterClientResponse::SharedMemNameSize - 1] = 0;
		response.shared_memory_size = sharedMemSink->MaxSharedMemSize;
	}
	else
		UE_LOG(LogDeepDriveClientConnection, Log, TEXT("PANIC: No SharedMemSink found"));


	response.max_supported_cameras = 8;
	response.max_capture_resolution = 2048;
	response.inactivity_timeout_ms = 40000;
	int32 bytesSent = 0;
	m_Socket->Send(reinterpret_cast<uint8*> (&response), sizeof(response), bytesSent);
	// UE_LOG(LogDeepDriveClientConnection, Log, TEXT("[%d] %d bytes sent back"), m_ClientId, bytesSent);
	m_isMaster = true;

	UE_LOG(LogDeepDriveClientConnection, Log, TEXT("[%d] Client registered."), m_ClientId);
}

void DeepDriveClientConnection::unregisterClient(const deepdrive::server::MessageHeader &message, bool isMaster)
{
	DeepDriveServer::GetInstance().unregisterClient(m_ClientId);

	const RegisterClientRequest &regClient = static_cast<const RegisterClientRequest &> (message);
	UE_LOG(LogDeepDriveClientConnection, Log, TEXT("[%d] Client wants to unregister isMaster %c"), m_ClientId, m_isMaster ? 'T' : 'F');
	UnregisterClientResponse response;
	int32 bytesSent = 0;
	m_Socket->Send(reinterpret_cast<uint8*> (&response), sizeof(response), bytesSent);
}

void DeepDriveClientConnection::shutdown()
{
	if (m_Socket)
	{
		const bool closed = m_Socket->Close();
		delete m_Socket;
		m_Socket = 0;

		UE_LOG(LogDeepDriveClientConnection, Log, TEXT("[%d] Closed connection successfully %c"), m_ClientId, closed ? 'T' : 'F');
	}
}


bool DeepDriveClientConnection::resizeReceiveBuffer(uint32 minSize)
{
	if (minSize > m_curReceiveBufferSize)
	{
		minSize += minSize / 4;
		delete m_ReceiveBuffer;
		m_ReceiveBuffer = reinterpret_cast<uint8*> (FMemory::Malloc(minSize));
		m_curReceiveBufferSize = minSize;
	}
	return m_ReceiveBuffer != 0;
}

void DeepDriveClientConnection::enqueueResponse(deepdrive::server::MessageHeader *message)
{
	if (message)
	{
		m_ResponseQueue.Enqueue(message);
	}
}
