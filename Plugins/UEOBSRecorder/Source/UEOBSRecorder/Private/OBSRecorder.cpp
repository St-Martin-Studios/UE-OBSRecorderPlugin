// Fill out your copyright notice in the Description page of Project Settings.


#include "OBSRecorder.h"
#include "IWebSocket.h"
#include "SHA256Hash.h"
#include "Misc/Base64.h"
#include "HashSHA256BPLibrary.h"
#include "JsonBlueprintFunctionLibrary.h"
#include "JsonObjectConverter.h"
#include "JsonObjectWrapper.h"
#include "OBSRecorderSettings.h"
#include "Containers/UnrealString.h"
#include "WebSocketsModule.h"
#include "Kismet/GameplayStatics.h"

DEFINE_LOG_CATEGORY(LogWebSocket);
DEFINE_LOG_CATEGORY(LogOBSRecorder);


void UOBSRecorder::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	//Load WebSocket module if already not
	if (!FModuleManager::Get().IsModuleLoaded("WebSockets"))
	{
		FModuleManager::Get().LoadModule("WebSockets");
		UE_LOG(LogWebSocket, Warning, TEXT("WS module is loaded!"));
	}

	//Get OBS Recorder settings
	const UOBSRecorderSettings* RecorderSettings = GetDefault<UOBSRecorderSettings>();
	if (!RecorderSettings) return;
	
	const FString Port = RecorderSettings->ServerPort;
	const FString URL = RecorderSettings->Host + Port;
	const FString Protocol = TEXT("ws");
	const FString Password = RecorderSettings->OBSWebSocketPassword;

	//Create websocket
	WebSocket = FWebSocketsModule::Get().CreateWebSocket(URL, Protocol);


	/*
	 * Initialize websocket events here.
	 */

	WebSocket->OnConnected().AddLambda([Port,Protocol]()
	{
		UE_LOG(LogWebSocket, Log, TEXT("Connected to websocket server succesfully: \n\tPort: %s\n\tProtocol: %s\n"),
		       *Port, *Protocol);
	});

	WebSocket->OnMessage().AddLambda([&,Password](const FString Message)
	{
		UE_LOG(LogOBSRecorder, Log, TEXT("Message received: %s"), *Message);

		FJsonObjectWrapper JsonObjectWrapper;
		UJsonBlueprintFunctionLibrary::FromString(nullptr,Message, JsonObjectWrapper); //Giving nullptr as an argument here. 
		const TSharedPtr<FJsonObject> OBSJsonResponse = JsonObjectWrapper.JsonObject;

		const FString MessageType = OBSJsonResponse->GetStringField("op");
		const TSharedPtr<FJsonObject> MessageData = OBSJsonResponse->GetObjectField("d");

		//Respond to OpCodes
		if (MessageType == FString::FromInt(OpCode0)) Identify(OBSJsonResponse, Password);
		else if (MessageType == FString::FromInt(OpCode2))
		{
			UE_LOG(LogOBSRecorder, Log,
			       TEXT(
				       "The identify request was received and validated, and the connection is now ready for normal operation."
			       ));
		}
		else if (MessageType == FString::FromInt(OpCode5))
		{
			FString Respond = FString::Printf(TEXT("%s"), *MessageData->GetStringField("eventType"));
		}
		else if (MessageType == FString::FromInt(OpCode7))
		{
			FString Respond = MessageData->GetObjectField("requestStatus")->GetBoolField("result")
				                  ? TEXT("Request successful!")
				                  : TEXT("Request unsuccessful!");

			UE_LOG(LogOBSRecorder, Log,
			       TEXT(
				       "%s"
			       ), *Respond);
		}
	});

	WebSocket->OnMessageSent().AddLambda([](const FString& MessageString)
	{
		UE_LOG(LogOBSRecorder, Log, TEXT("Message sent: %s"), *MessageString);
	});

	WebSocket->OnConnectionError().AddLambda([Port,Protocol](const FString& ErrorMessage)
	{
		UE_LOG(LogWebSocket, Error,
		       TEXT("Failed to connect to WebSocket server: \n\tPort: %s\n\tProtocol: %s\n\tError Message: %s\n"),
		       *Port, *Protocol, *ErrorMessage);
		UE_LOG(LogWebSocket, Error,
			   TEXT("Please check your plugin and obs-websocket settings."));
	});

	WebSocket->OnClosed().AddLambda([Port,Protocol](int32 StatusCode, const FString& Reason, bool bWasClean)
	{
		UE_LOG(LogWebSocket, Display,
		       TEXT(
			       "WebSocket connection closed: \n\tPort: %s\n\tProtocol: %s\n\tStatus Code: %d\n\tReason: %s\n\tWas Clean: %d\n"
		       ),
		       *Port, *Protocol, StatusCode, *Reason, bWasClean);
	});

	
	
}

void UOBSRecorder::Deinitialize()
{
	if (WebSocket->IsConnected())
	{
		//Stop recording as we are closing the websocket connection. TODO: Currently does not work.
		WebSocket->Close();
	}
	Super::Deinitialize();
}




void UOBSRecorder::StartConnection(bool& Success)
{
	if (!WebSocket->IsConnected())
	{
		WebSocket->Connect();
	}
}

void UOBSRecorder::Identify(const TSharedPtr<FJsonObject> HelloMessageJson, const FString& Password)
{
	UE_LOG(LogOBSRecorder, Log, TEXT("Hello OBSWebsocket!"));
	UE_LOG(LogOBSRecorder, Log, TEXT("Generating authenticator key and verifying client..."));

	//Get challenge field
	const FString Challenge = HelloMessageJson->GetObjectField("d")->GetObjectField("authentication")->GetStringField(
		"challenge");

	//Get salt field
	const FString Salt = HelloMessageJson->GetObjectField("d")->GetObjectField("authentication")->
										   GetStringField("salt");

	const FString AuthenticationKey = GenerateAuthenticationKey(Password, Salt, Challenge);

	//Create Identify (OpCode 1) message

	const TSharedPtr<FJsonObject> IdentifyJsonObject = MakeShareable(new FJsonObject);
	//TODO: Can you use TMaps here for convenience ??
	IdentifyJsonObject->SetNumberField(TEXT("rpcVersion"),1);
	IdentifyJsonObject->SetStringField(TEXT("authentication"),AuthenticationKey);
	IdentifyJsonObject->SetNumberField(TEXT("eventSubscriptions"),33);
	

	WebSocket->Send(FormJsonMessage(OpCode1,IdentifyJsonObject)); //Sends 
}

void UOBSRecorder::MakeRecordRequest(const ERecordRequest RecordRequest)
{
	const FString Request = UEnum::GetDisplayValueAsText(RecordRequest).ToString();
	WebSocket->Send(MakeRequestJsonObject(Request,TMap<FString,FString>()));
}

void UOBSRecorder::ToggleInputMute(const FString& InputName)
{
	TMap<FString,FString> Map;
	Map.Add(TEXT("inputName"),InputName);
	WebSocket->Send(MakeRequestJsonObject(TEXT("ToggleInputMute"),Map));
}

void UOBSRecorder::GetProfileParameter(const FString& parameterCategory,const FString& parameterName)
{
	TMap<FString,FString> Map;
	Map.Add(TEXT("parameterCategory"),parameterCategory);
	Map.Add(TEXT("parameterName"),parameterName);
	WebSocket->Send(MakeRequestJsonObject(TEXT("GetProfileParameter"),Map));
}

void UOBSRecorder::SetRecordDirectory(const FString& Directory, const FString& FileName)
{
	TMap<FString,FString> Map;
	Map.Add(TEXT("parameterValue"),Directory);
	Map.Add(TEXT("parameterName"),"FilePath");
	Map.Add(TEXT("parameterCategory"),"SimpleOutput");
	
	WebSocket->Send(MakeRequestJsonObject(TEXT("SetProfileParameter"),Map));


	TMap<FString,FString> Map2;
	Map2.Add(TEXT("parameterValue"),FileName);
	Map2.Add(TEXT("parameterName"),"FilenameFormatting");
	Map2.Add(TEXT("parameterCategory"),"Output"); //As opposed to above parameterCategory this should be "Output".
	WebSocket->Send(MakeRequestJsonObject(TEXT("SetProfileParameter"),Map2));
}

void UOBSRecorder::MakeGetRequest(const FString& Request)
{
	WebSocket->Send(MakeRequestJsonObject(Request,TMap<FString,FString>()));
}





const FString UOBSRecorder::FormJsonMessage(const EClientRequest OpCode, TSharedPtr<FJsonObject> DataJsonObject)
{
	TSharedRef<FJsonObject> JsonObject = MakeShareable(new FJsonObject);
	//Add first level fields
	JsonObject->SetNumberField(TEXT("op"),OpCode);
	JsonObject->SetObjectField(TEXT("d"),DataJsonObject);

	FString OutputJsonString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputJsonString);
	FJsonSerializer::Serialize(JsonObject,Writer);

	return OutputJsonString;
}

const FString UOBSRecorder::MakeRequestJsonObject(const FString RequestType,const TMap<FString, FString>& StringField)
{
	const TSharedPtr<FJsonObject> RequestJsonObject = MakeShareable(new FJsonObject);
	RequestJsonObject->SetStringField(TEXT("requestType"),RequestType);
	RequestJsonObject->SetStringField(TEXT("requestId"),FGuid::NewGuid().ToString());

	const TSharedPtr<FJsonObject> RequestDataJsonObject = MakeShareable(new FJsonObject);
	RequestJsonObject->SetObjectField(TEXT("requestData"),RequestDataJsonObject);
	if (!StringField.IsEmpty())
	{
		for (auto n: StringField)
		{
			RequestDataJsonObject->SetStringField(n.Key,n.Value);
		}
	}
	
	return FormJsonMessage(OpCode6,RequestJsonObject);
}


FString UOBSRecorder::GenerateAuthenticationKey(const FString& Password, const FString& Salt, const FString& Challenge)
{
	//Concatenate the websocket password with the salt provided by the server (password + salt)
	FString SecretString = Password + Salt;
	//UE_LOG(LogTemp,Error,TEXT("Password + salt: %s"), *SecretString);

	//Generate an SHA256 binary hash of the result and base64 encode it, known as a base64 secret
	FSHA256Hash Fsha256Hash;
	UHashSHA256BPLibrary::FromString(Fsha256Hash, SecretString);
	FString SHA256 = Fsha256Hash.GetHash();
	//UE_LOG(LogTemp,Error,TEXT("SHA256: %s"), *SHA256);

	//Concatenate the base64 secret with the challenge sent by the server (base64_secret + challenge)
	SecretString = HexToBase64(SHA256) + Challenge;

	//Generate a binary SHA256 hash of that result and base64 encode it. You now have your authentication string.
	UHashSHA256BPLibrary::FromString(Fsha256Hash, SecretString);
	SHA256 = Fsha256Hash.GetHash();
	SecretString = HexToBase64(SHA256);

	return SecretString;
}

FString UOBSRecorder::HexToBase64(FString& HexString)
{
	uint8* Source = new uint8[32]; //TODO: Fix 
	const uint32 Length = HexToBytes(HexString, Source); //Decode Hex String to byte arrays

	HexString = FBase64::Encode(Source, Length); //Encode Base64

	delete[] Source;
	return HexString;
}
