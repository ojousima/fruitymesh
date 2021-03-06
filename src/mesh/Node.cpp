////////////////////////////////////////////////////////////////////////////////
// /****************************************************************************
// **
// ** Copyright (C) 2015-2020 M-Way Solutions GmbH
// ** Contact: https://www.blureange.io/licensing
// **
// ** This file is part of the Bluerange/FruityMesh implementation
// **
// ** $BR_BEGIN_LICENSE:GPL-EXCEPT$
// ** Commercial License Usage
// ** Licensees holding valid commercial Bluerange licenses may use this file in
// ** accordance with the commercial license agreement provided with the
// ** Software or, alternatively, in accordance with the terms contained in
// ** a written agreement between them and M-Way Solutions GmbH. 
// ** For licensing terms and conditions see https://www.bluerange.io/terms-conditions. For further
// ** information use the contact form at https://www.bluerange.io/contact.
// **
// ** GNU General Public License Usage
// ** Alternatively, this file may be used under the terms of the GNU
// ** General Public License version 3 as published by the Free Software
// ** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
// ** included in the packaging of this file. Please review the following
// ** information to ensure the GNU General Public License requirements will
// ** be met: https://www.gnu.org/licenses/gpl-3.0.html.
// **
// ** $BR_END_LICENSE$
// **
// ****************************************************************************/
////////////////////////////////////////////////////////////////////////////////

#include <Config.h>

#include <Node.h>
#include <LedWrapper.h>
#include <AdvertisingController.h>
#include <GAPController.h>
#include <GlobalState.h>
#include <GATTController.h>
#include <ConnectionManager.h>
#include <ScanController.h>
#include <Utility.h>
#include <Logger.h>
#include <StatusReporterModule.h>
#include <MeshConnection.h>
#include <MeshAccessConnection.h>
#include "MeshAccessModule.h"
#include "mini-printf.h"

#include <ctime>
#include <cmath>
#include <cstdlib>

#ifdef SIM_ENABLED
#include <CherrySim.h>	//required for faking DFU
#endif

constexpr u8 NODE_MODULE_CONFIG_VERSION = 2;

//The number of connection attempts to one node before blacklisting this node for some time
constexpr u8 connectAttemptsBeforeBlacklisting = 5;

// The Service that is used for two nodes to communicate between each other
// Fruity Mesh Service UUID 310bfe40-ed6b-11e3-a1be-0002a5d5c51b
constexpr u8 MESH_SERVICE_BASE_UUID128[] = { 0x23, 0xD1, 0xBC, 0xEA, 0x5F, 0x78, 0x23, 0x15, 0xDE, 0xEF, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00 };
constexpr u16 MESH_SERVICE_CHARACTERISTIC_UUID = 0x1524;

Node::Node()
	: Module(ModuleId::NODE, "node")
{
	CheckedMemset(&meshService, 0, sizeof(meshService));

	//Save configuration to base class variables
	//sizeof configuration must be a multiple of 4 bytes
	configurationPointer = &configuration;
	configurationLength = sizeof(NodeConfiguration);
}

void Node::Init()
{
	//Load default configuration
	ResetToDefaultConfiguration();
	isInit = true;
}

bool Node::IsInit()
{
	return isInit;
}

void Node::ResetToDefaultConfiguration()
{
	configuration.moduleId = ModuleId::NODE;
	configuration.moduleVersion = NODE_MODULE_CONFIG_VERSION;
	configuration.moduleActive = true;

	//Load defaults from Config
	configuration.enrollmentState = RamConfig->defaultNetworkId != 0 ? EnrollmentState::ENROLLED : EnrollmentState::NOT_ENROLLED;
	configuration.nodeId = RamConfig->defaultNodeId;
	configuration.networkId = RamConfig->defaultNetworkId;
	CheckedMemcpy(configuration.networkKey, RamConfig->defaultNetworkKey, 16);
	CheckedMemcpy(configuration.userBaseKey, RamConfig->defaultUserBaseKey, 16);

	CheckedMemcpy(&configuration.bleAddress, &RamConfig->staticAccessAddress, sizeof(FruityHal::BleGapAddr));

	SET_FEATURESET_CONFIGURATION(&configuration, this);
}

void Node::ConfigurationLoadedHandler(ModuleConfiguration* migratableConfig, u16 migratableConfigLength)
{
	//We must now decide if we want to overwrite some unset persistent config values with defaults
	if(configuration.nodeId == 0) configuration.nodeId = RamConfig->defaultNodeId;
	if(configuration.networkId == 0) configuration.networkId = RamConfig->defaultNetworkId;
	if(Utility::CompareMem(0x00, configuration.networkKey, 16)){
		CheckedMemcpy(configuration.networkKey, RamConfig->defaultNetworkKey, 16);
	}
	if(Utility::CompareMem(0x00, configuration.userBaseKey, 16)){
		CheckedMemcpy(configuration.userBaseKey, RamConfig->defaultUserBaseKey, 16);
	}

	//Random offset that can be used to disperse packets from different nodes over time
	GS->appTimerRandomOffsetDs = (configuration.nodeId % 100);

	//Change window title of the Terminal
	SetTerminalTitle();
	logt("NODE", "====> Node %u (%s) <====", configuration.nodeId, RamConfig->GetSerialNumber());

	//Get a random number for the connection loss counter (hard on system start,...stat)
	randomBootNumber = Utility::GetRandomInteger();

	clusterId = this->GenerateClusterID();

	//Set the BLE address so that we have the same on every startup, mostly for debugging
	if(configuration.bleAddress.addr_type != FruityHal::BleGapAddrType::INVALID){
		ErrorType err = FruityHal::SetBleGapAddress(configuration.bleAddress);
		if(err != ErrorType::SUCCESS){
			//Can be ignored and will not happen
		}
	}

	//Print configuration and start node
	logt("NODE", "Config loaded nodeId:%d, connLossCount:%u, networkId:%d", configuration.nodeId, connectionLossCounter, configuration.networkId);

	//Register the mesh service in the GATT table
	InitializeMeshGattService();

	//Remove Advertising job if it's been registered before
	GS->advertisingController.RemoveJob(meshAdvJobHandle);
	meshAdvJobHandle = nullptr;


	if(GET_DEVICE_TYPE() != DeviceType::ASSET && configuration.networkId != 0){
		//Register Job with AdvertisingController
		AdvJob job = {
			AdvJobTypes::SCHEDULED,
			5, //Slots
			0, //Delay
			MSEC_TO_UNITS(100, CONFIG_UNIT_0_625_MS), //AdvInterval
			0, //AdvChannel
			0, //CurrentSlots
			0, //CurrentDelay
			FruityHal::BleGapAdvType::ADV_IND, //Advertising Mode
			{0}, //AdvData
			0, //AdvDataLength
			{0}, //ScanData
			0 //ScanDataLength
		};
		meshAdvJobHandle = GS->advertisingController.AddJob(job);

		//Go to Discovery if node is active
		//Fill JOIN_ME packet with data
		this->UpdateJoinMePacket();

		ChangeState(DiscoveryState::HIGH);
	}
}

void Node::InitializeMeshGattService()
{
	u32 err = 0;

	//##### At first, we register our custom service
	//Add our Service UUID to the BLE stack for management
	err = (u32)FruityHal::BleUuidVsAdd(MESH_SERVICE_BASE_UUID128, &meshService.serviceUuid.type);
	FRUITYMESH_ERROR_CHECK(err); //OK

	//Add the service
	err = (u32)FruityHal::BleGattServiceAdd(FruityHal::BleGattSrvcType::PRIMARY, meshService.serviceUuid, &meshService.serviceHandle);
	FRUITYMESH_ERROR_CHECK(err); //OK

	//##### Now we need to add a characteristic to that service

	//BLE GATT Attribute Metadata http://developer.nordicsemi.com/nRF51_SDK/doc/7.1.0/s120/html/a00163.html
	//Read and write permissions, variable length, etc...
	FruityHal::BleGattAttributeMetadata attributeMetadata;
	CheckedMemset(&attributeMetadata, 0, sizeof(attributeMetadata));

	//If encryption is enabled, we want our mesh handle only to be accessable over an
	//encrypted connection with authentication
	if(Conf::encryptionEnabled){
		FH_CONNECTION_SECURITY_MODE_SET_ENC_NO_MITM(&attributeMetadata.readPerm);
		FH_CONNECTION_SECURITY_MODE_SET_ENC_NO_MITM(&attributeMetadata.writePerm);
	}
	else
	{
		FH_CONNECTION_SECURITY_MODE_SET_OPEN(&attributeMetadata.readPerm);
		FH_CONNECTION_SECURITY_MODE_SET_OPEN(&attributeMetadata.writePerm);
	}

	attributeMetadata.valueLocation = FH_BLE_GATTS_VALUE_LOCATION_STACK; //We currently have the value on the SoftDevice stack, we might port that to the application space
	attributeMetadata.readAuthorization = 0;
	attributeMetadata.writeAuthorization = 0;
	attributeMetadata.variableLength = 1; //Make it a variable length attribute

	//Characteristic metadata, whatever....
	FruityHal::BleGattCharMd characteristicMetadata;
	CheckedMemset(&characteristicMetadata, 0, sizeof(characteristicMetadata));
	characteristicMetadata.charProperties.read = 1; /*Reading value permitted*/
	characteristicMetadata.charProperties.write = 1; /*Writing value with Write Request permitted*/
	characteristicMetadata.charProperties.writeWithoutResponse = 1; /*Writing value with Write Command permitted*/
	characteristicMetadata.charProperties.authSignedWrite = 0; /*Writing value with Signed Write Command not permitted*/
	characteristicMetadata.charProperties.notify = 1; /*Notications of value permitted*/
	characteristicMetadata.charProperties.indicate = 0; /*Indications of value not permitted*/
	characteristicMetadata.p_cccdMd = nullptr;

	//Finally, the attribute
	FruityHal::BleGattAttribute attribute;
	CheckedMemset(&attribute, 0, sizeof(attribute));

	FruityHal::BleGattUuid attributeUUID;
	attributeUUID.type = meshService.serviceUuid.type;
	attributeUUID.uuid = MESH_SERVICE_CHARACTERISTIC_UUID;

	attribute.p_uuid = &attributeUUID; /* The UUID of the Attribute*/
	attribute.p_attributeMetadata = &attributeMetadata; /* The previously defined attribute Metadata */
	attribute.maxLen = MESH_CHARACTERISTIC_MAX_LENGTH;
	attribute.initLen = 0;
	attribute.initOffset = 0;

	//Finally, add the characteristic
	err = (u32)FruityHal::BleGattCharAdd(meshService.serviceHandle, characteristicMetadata, attribute, meshService.sendMessageCharacteristicHandle);
	FRUITYMESH_ERROR_CHECK(err); //OK
}


/*
 #########################################################################################################
 ### Connections and Handlers
 #########################################################################################################
 */
#define ________________CONNECTION___________________

//Is called after a connection has ended its handshake
void Node::HandshakeDoneHandler(MeshConnection* connection, bool completedAsWinner)
{
	logt("HANDSHAKE", "############ Handshake done (asWinner:%u) ###############", completedAsWinner);

	StatusReporterModule* statusMod = (StatusReporterModule*)GS->node.GetModuleById(ModuleId::STATUS_REPORTER_MODULE);
	if(statusMod != nullptr){
		statusMod->SendLiveReport(LiveReportTypes::MESH_CONNECTED, 0, connection->partnerId, completedAsWinner);
	}

	GS->logger.logCustomCount(CustomErrorTypes::COUNT_HANDSHAKE_DONE);

	//We delete the joinMe packet of this node from the join me buffer
	for (u32 i = 0; i < joinMePackets.size(); i++)
	{
		joinMeBufferPacket* packet = &joinMePackets[i];
		if (packet->payload.sender == connection->partnerId) {
			CheckedMemset(packet, 0x00, sizeof(joinMeBufferPacket));
		}
	}

	//We can now commit the changes that were part of the handshake
	//This node was the winner of the handshake and successfully acquired a new member
	if(completedAsWinner){
		//Update node data
		clusterSize += 1;
		connection->hopsToSink = connection->clusterAck1Packet.payload.hopsToSink < 0 ? -1 : connection->clusterAck1Packet.payload.hopsToSink + 1;

		logt("HANDSHAKE", "ClusterSize Change from %d to %d", clusterSize-1, clusterSize);

		//Update connection data
		connection->connectedClusterId = connection->clusterIDBackup;
		connection->partnerId = connection->clusterAck1Packet.header.sender;
		connection->connectedClusterSize = 1;

		//Broadcast cluster update to other connections
		connPacketClusterInfoUpdate outPacket;
		CheckedMemset((u8*)&outPacket, 0x00, sizeof(connPacketClusterInfoUpdate));

		outPacket.payload.clusterSizeChange = 1;
		outPacket.payload.connectionMasterBitHandover = 0;
		// => hops to sink is set later in SendClusterInfoUpdate

		SendClusterInfoUpdate(connection, &outPacket);

	//This node was the looser of the Handshake and is now part of a newer bigger cluster
	} else {

		//The node that receives this message can not be connected to any other node
		//This is why we can set absolute values for the clusterSize
		connection->connectedClusterId = connection->clusterAck2Packet.payload.clusterId;
		connection->connectedClusterSize = connection->clusterAck2Packet.payload.clusterSize - 1; // minus myself

		//If any cluster updates are waiting, we delete them
		connection->ClearCurrentClusterInfoUpdatePacket();

		clusterId = connection->clusterAck2Packet.payload.clusterId;
		clusterSize = connection->clusterAck2Packet.payload.clusterSize; // The other node knows best

		connection->hopsToSink = connection->clusterAck2Packet.payload.hopsToSink < 0 ? -1 : connection->clusterAck2Packet.payload.hopsToSink + 1;

		logt("HANDSHAKE", "ClusterSize set to %d", clusterSize);
	}

	logjson("CLUSTER", "{\"type\":\"cluster_handshake\",\"winner\":%u,\"size\":%d}" SEP, completedAsWinner, clusterSize);

	logjson("SIM", "{\"type\":\"mesh_connect\",\"partnerId\":%u}" SEP, connection->partnerId);

	connection->connectionState = ConnectionState::HANDSHAKE_DONE;
	connection->connectionHandshakedTimestampDs = GS->appTimerDs;

	// Send ClusterInfo again as the amount of hops to the sink will have changed
	// after this connection is in the handshake done state
	// TODO: This causes an increase in cluster info update packets. It is possible to combine this with
	//the cluster update above, but that requires more debugging to get it correctly working
	SendClusterInfoUpdate(connection, nullptr);

	//Call our lovely modules
	for(u32 i=0; i<GS->amountOfModules; i++){
		if(GS->activeModules[i]->configurationPointer->moduleActive){
			GS->activeModules[i]->MeshConnectionChangedHandler(*connection);
		}
	}

	//Enable discovery or prolong its state
	KeepHighDiscoveryActive();

	//Update our advertisement packet
	UpdateJoinMePacket();

	//Pass on the masterbit to someone if necessary
	HandOverMasterBitIfNecessary();
}

MeshAccessAuthorization Node::CheckMeshAccessPacketAuthorization(BaseConnectionSendData * sendData, u8 const * data, FmKeyId fmKeyId, DataDirection direction)
{
	connPacketHeader const * packet = (connPacketHeader const *)data;
	
	if (   packet->messageType == MessageType::MODULE_RAW_DATA
		|| packet->messageType == MessageType::MODULE_RAW_DATA_LIGHT)
	{
		if (fmKeyId == FmKeyId::NETWORK)
		{
			return MeshAccessAuthorization::WHITELIST;
		}
		else if (fmKeyId == FmKeyId::NODE)
		{
			return MeshAccessAuthorization::LOCAL_ONLY;
		}
	}
	if (packet->messageType == MessageType::CLUSTER_INFO_UPDATE)
	{
		if (fmKeyId == FmKeyId::NETWORK)
		{
			return MeshAccessAuthorization::WHITELIST;
		}
		else
		{
			return MeshAccessAuthorization::UNDETERMINED;
		}
	}
	if (packet->messageType == MessageType::UPDATE_TIMESTAMP)
	{
		//Don't allow the time to be set if it's already set and we didn't receive this message via FM_KEY_ID_NETWORK.
		//Note: FM_KEY_ID_NODE is not sufficient, as the time is a property of the mesh by design.
		if (GS->timeManager.IsTimeSynced() && fmKeyId != FmKeyId::NETWORK)
		{
			return MeshAccessAuthorization::BLACKLIST;
		}
		else
		{
			return MeshAccessAuthorization::WHITELIST;
		}
	}
	if (   packet->messageType == MessageType::COMPONENT_SENSE
	    || packet->messageType == MessageType::CAPABILITY)
	{
		if (fmKeyId == FmKeyId::ORGANIZATION)
		{
			return MeshAccessAuthorization::WHITELIST;
		}
	}
	return MeshAccessAuthorization::UNDETERMINED;
}

//TODO: part of the connection manager
//void Node::HandshakeTimeoutHandler()
//{
//	logt("HANDSHAKE", "############ Handshake TIMEOUT/FAIL ###############");
//
//	//Disconnect the hanging connection
//	BaseConnections conn = GS->cm.GetBaseConnections(ConnectionDirection::INVALID);
//	for(int i=0; i<conn.count; i++){
//		if(conn.connections[i]->isConnected() && !conn.connections[i]->handshakeDone()){
//			u32 handshakeTimePassed = GS->appTimerDs - conn.connections[i]->handshakeStartedDs;
//			logt("HANDSHAKE", "Disconnecting conn %u, timePassed:%u", conn.connections[i]->connectionId, handshakeTimePassed);
//			conn.connections[i]->Disconnect();
//		}
//	}
//
//	//Go back to discovery
//	ChangeState(discoveryState::DISCOVERY);
//}


void Node::MeshConnectionDisconnectedHandler(AppDisconnectReason appDisconnectReason, ConnectionState connectionStateBeforeDisconnection, u8 hadConnectionMasterBit, i16 connectedClusterSize, u32 connectedClusterId)
{
	logt("NODE", "MeshConn Disconnected with previous state %u", (u32)connectionStateBeforeDisconnection);

	//TODO: If the local host disconnected this connection, it was already increased, we do not have to count the disconnect here
	this->connectionLossCounter++;

	//If the handshake was already done, this node was part of our cluster
	//If the local host terminated the connection, we do not count it as a cluster Size change
	if (
		connectionStateBeforeDisconnection >= ConnectionState::HANDSHAKE_DONE
	){
		//CASE 1: if our partner has the connection master bit, we must dissolve
		//It may happen rarely that the connection master bit was just passed over and that neither node has it
		//This will result in two clusters dissolving
		if (!hadConnectionMasterBit)
		{
			//FIXME: Workaround to not clean up the wrong connections because in this case, all connections are already cleaned up
			if (appDisconnectReason != AppDisconnectReason::I_AM_SMALLER) {
				GS->cm.ForceDisconnectOtherMeshConnections(nullptr, AppDisconnectReason::PARTNER_HAS_MASTERBIT);
			}

			clusterSize = 1;
			clusterId = GenerateClusterID();


			SendClusterInfoUpdate(nullptr, nullptr);
		}

		//CASE 2: If we have the master bit, we keep our ClusterId (happens if we are the biggest cluster)
		else
		{
			logt("HANDSHAKE", "ClusterSize Change from %d to %d", this->clusterSize, this->clusterSize - connectedClusterSize);

			this->clusterSize -= connectedClusterSize;

			// Inform the rest of the cluster of our new size
			connPacketClusterInfoUpdate packet;
			CheckedMemset((u8*)&packet, 0x00, sizeof(connPacketClusterInfoUpdate));

			packet.payload.clusterSizeChange = -connectedClusterSize;

			SendClusterInfoUpdate(nullptr, &packet);

		}

		logjson("CLUSTER", "{\"type\":\"cluster_disconnect\",\"size\":%d}" SEP, clusterSize);

	}
	//Handshake had not yet finished, not much to do
	else
	{

	}

	//Enable discovery or prolong its state
	KeepHighDiscoveryActive();

	//To be sure we do not have a clusterId clash if we are disconnected, we generate one if we are a single node, doesn't hurt
	if (clusterSize == 1) clusterId = GenerateClusterID();

	//In either case, we must update our advertising packet
	UpdateJoinMePacket();

	//Pass on the masterbit to someone if necessary
	HandOverMasterBitIfNecessary();

	//Revert to discovery high
	noNodesFoundCounter = 0;

	disconnectTimestampDs = GS->appTimerDs;
	//TODO: Under some conditions, broadcast a message to the mesh to activate HIGH discovery again
}

//Handles incoming cluster info update
void Node::ReceiveClusterInfoUpdate(MeshConnection* connection, connPacketClusterInfoUpdate const * packet)
{
	//Check if next expected counter matches, if not, this clusterUpdate was a duplicate and we ignore it (might happen during reconnection)
	if (connection->nextExpectedClusterUpdateCounter == packet->payload.counter) {
		connection->nextExpectedClusterUpdateCounter++;
	}
	else {
		//This must not happen normally, only in rare cases where the connection is reestablished and the remote node receives a duplicate of the cluster update message
		SIMSTATCOUNT("ClusterUpdateCountMismatch");
		logt("ERROR", "Next expected ClusterUpdateCounter did not match");
		GS->logger.logCustomError(CustomErrorTypes::FATAL_CLUSTER_UPDATE_FLOW_MISMATCH, connection->partnerId);
		return;
	}

	SIMSTATCOUNT("ClusterUpdateCount");

	//Prepare cluster update packet for other connections
	connPacketClusterInfoUpdate outPacket;
	CheckedMemset((u8*)&outPacket, 0x00, sizeof(connPacketClusterInfoUpdate));
	outPacket.payload.clusterSizeChange = packet->payload.clusterSizeChange;


	if(packet->payload.clusterSizeChange != 0){
		logt("HANDSHAKE", "ClusterSize Change from %d to %d", this->clusterSize, this->clusterSize + packet->payload.clusterSizeChange);
		this->clusterSize += packet->payload.clusterSizeChange;
		connection->connectedClusterSize += packet->payload.clusterSizeChange;
	}

	//Update hops to sink
	//Another sink may have joined or left the network, update this
	//FIXME: race conditions can cause this to work incorrectly...
	connection->hopsToSink = packet->payload.hopsToSink > -1 ? packet->payload.hopsToSink + 1 : -1;
	
	//Now look if our partner has passed over the connection master bit
	if(packet->payload.connectionMasterBitHandover){
		logt("CONN", "NODE %u RECEIVED MASTERBIT FROM %u", configuration.nodeId, packet->header.sender);
		connection->connectionMasterBit = 1;
	}

	//Pass on the masterbit to someone else if necessary
	HandOverMasterBitIfNecessary();

	//hops to sink are updated in the send method
	//current cluster id is updated in the send method

	SendClusterInfoUpdate(connection, &outPacket);

	//Log Cluster change to UART
	logjson("CLUSTER", "{\"type\":\"cluster_update\",\"size\":%d,\"newId\":%u,\"masterBit\":%u}" SEP, clusterSize, clusterId, packet->payload.connectionMasterBitHandover);

	//Enable discovery or prolong its state
	KeepHighDiscoveryActive();

	//Update adverting packet
	this->UpdateJoinMePacket();

	//TODO: What happens if:
	/*
	 * We send a clusterid update and commit it in our connection arm
	 * The other one does the same at nearly the same time
	 * ID before was 3, A now has 2 and 2 on the connection arm, B has 4 and 4 on the connection arm
	 * Then both will not accept the new ClusterId!!!
	 * What if the biggest id will always win?
	 */
}

void Node::HandOverMasterBitIfNecessary()  const{
	//If we have all masterbits, we can give 1 at max
	//We do this, if the connected cluster size is bigger than all the other connected cluster sizes summed together
	bool hasAllMasterBits = HasAllMasterBits();
	if (hasAllMasterBits) {
		MeshConnections conns = GS->cm.GetMeshConnections(ConnectionDirection::INVALID);
		for (u32 i = 0; i < conns.count; i++) {
			MeshConnectionHandle conn = conns.handles[i];
			if (conn.IsHandshakeDone() && conn.GetConnectedClusterSize() > clusterSize - conn.GetConnectedClusterSize()) {
				conn.HandoverMasterBit();
			}
		}
	}
}

bool Node::HasAllMasterBits() const {
	MeshConnections conn = GS->cm.GetMeshConnections(ConnectionDirection::INVALID);
	for (u32 i = 0; i < conn.count; i++) {
		MeshConnectionHandle connection = conn.handles[i];
		//Connection must be handshaked, if yes check if we have its masterbit
		if (connection.IsHandshakeDone() && !connection.HasConnectionMasterBit()) {
			return false;
		}
	}
	return true;
}



//Saves a cluster update for all connections (except the one that caused it)
//This update will then be sent by a connection as soon as the connection is ready (handshakeDone)
void Node::SendClusterInfoUpdate(MeshConnection* ignoreConnection, connPacketClusterInfoUpdate* packet) const
{
	MeshConnections conn = GS->cm.GetMeshConnections(ConnectionDirection::INVALID);
	for (u32 i = 0; i < conn.count; i++) {
		if (!conn.handles[i]) continue;

		//Get the current packet
		connPacketClusterInfoUpdate* currentPacket = &(conn.handles[i].GetConnection()->currentClusterInfoUpdatePacket);

		if(!conn.handles[i].GetConnection()->isConnected()) continue;

		//We currently update the hops to sink at all times
		currentPacket->payload.hopsToSink = GS->cm.GetMeshHopsToShortestSink(conn.handles[i].GetConnection());

		if (conn.handles[i].GetConnection() == ignoreConnection) continue;
		
		if (packet != nullptr) {
			currentPacket->payload.clusterSizeChange += packet->payload.clusterSizeChange;
		}
		
		//=> The counter and maybe some other fields are set right before queuing the packet

		logt("HANDSHAKE", "OUT => %u MESSAGE_TYPE_CLUSTER_INFO_UPDATE clustChange:%d, hops:%d", conn.handles[i].GetPartnerId(), currentPacket->payload.clusterSizeChange, currentPacket->payload.hopsToSink);
	}

	HandOverMasterBitIfNecessary();

	//Send the current state of our cluster to all active MeshAccess connections
	MeshAccessConnections conns2 = GS->cm.GetMeshAccessConnections(ConnectionDirection::INVALID);
	for (u32 i = 0; i < conns2.count; i++) {
		MeshAccessConnectionHandle conn = conns2.handles[i];
		if (conn && conn.IsHandshakeDone()) {
			conn.SendClusterState();
		}
	}

	//TODO: If we call fillTransmitBuffers after a timeout, they would accumulate more,...
	GS->cm.fillTransmitBuffers();
}

void Node::MeshMessageReceivedHandler(BaseConnection* connection, BaseConnectionSendData* sendData, connPacketHeader const * packetHeader)
{
	//Must call superclass for handling
	Module::MeshMessageReceivedHandler(connection, sendData, packetHeader);

	//If the packet is a handshake packet it will not be forwarded to the node but will be
	//handled in the connection. All other packets go here for further processing
	switch (packetHeader->messageType)
	{
		case MessageType::CLUSTER_INFO_UPDATE:
			if (
					connection != nullptr
					&& connection->connectionType == ConnectionType::FRUITYMESH
					&& sendData->dataLength >= SIZEOF_CONN_PACKET_CLUSTER_INFO_UPDATE)
			{
				connPacketClusterInfoUpdate const * packet = (connPacketClusterInfoUpdate const *) packetHeader;
				logt("HANDSHAKE", "IN <= %d CLUSTER_INFO_UPDATE sizeChange:%d, hop:%d", connection->partnerId, packet->payload.clusterSizeChange, packet->payload.hopsToSink);
				ReceiveClusterInfoUpdate((MeshConnection*)connection, packet);

			}
			break;
#if IS_INACTIVE(SAVE_SPACE)
		case MessageType::UPDATE_CONNECTION_INTERVAL:
			if(sendData->dataLength == SIZEOF_CONN_PACKET_UPDATE_CONNECTION_INTERVAL)
			{
				connPacketUpdateConnectionInterval const * packet = (connPacketUpdateConnectionInterval const *) packetHeader;

				GS->cm.SetMeshConnectionInterval(packet->newInterval);
			}
			break;
#endif
		default:	//Surpress GCC warning of unhandled MessageTypes
			break;
	}


	if(packetHeader->messageType == MessageType::MODULE_CONFIG)
	{
		connPacketModule const * packet = (connPacketModule const *) packetHeader;

		if(packet->actionType == (u8)Module::ModuleConfigMessages::GET_MODULE_LIST)
		{
			SendModuleList(packet->header.sender, packet->requestHandle);

		}
#if IS_INACTIVE(SAVE_SPACE)
		else if(packet->actionType == (u8)Module::ModuleConfigMessages::MODULE_LIST)
		{
			logjson_partial("MODULE", "{\"nodeId\":%u,\"type\":\"module_list\",\"modules\":[", packet->header.sender);

			u16 moduleCount = (sendData->dataLength - SIZEOF_CONN_PACKET_MODULE) / 4;
			for(int i=0; i<moduleCount; i++){
				ModuleId moduleId;
				u8 version = 0, active = 0;
				CheckedMemcpy(&moduleId, packet->data + i*4+0, 1);
				CheckedMemcpy(&version, packet->data + i*4+2, 1);
				CheckedMemcpy(&active, packet->data + i*4+3, 1);

				if(i > 0){
					logjson_partial("MODULE", ",");
				}
				logjson_partial("MODULE", "{\"id\":%u,\"version\":%u,\"active\":%u}", (u32)moduleId, version, active);
			}
			logjson("MODULE", "]}" SEP);
		}
#endif
	}


	if(packetHeader->messageType == MessageType::MODULE_TRIGGER_ACTION){
		connPacketModule const * packet = (connPacketModule const *)packetHeader;

		//Check if our module is meant and we should trigger an action
		if(packet->moduleId == ModuleId::NODE){


			if(packet->actionType == (u8)NodeModuleTriggerActionMessages::SET_DISCOVERY){

				u8 ds = packet->data[0];

				if(ds == 0){
					ChangeState(DiscoveryState::OFF);
				} else {
					ChangeState(DiscoveryState::HIGH);
				}

				SendModuleActionMessage(
					MessageType::MODULE_ACTION_RESPONSE,
					packetHeader->sender,
					(u8)NodeModuleActionResponseMessages::SET_DISCOVERY_RESULT,
					0,
					nullptr,
					0,
					false
				);
			}

			if (packet->actionType == (u8)NodeModuleTriggerActionMessages::PING) {
				SendModuleActionMessage(
					MessageType::MODULE_ACTION_RESPONSE,
					packetHeader->sender,
					(u8)NodeModuleActionResponseMessages::PING,
					packet->requestHandle,
					nullptr,
					0,
					false
				);
			}

			else if (packet->actionType == (u8)NodeModuleTriggerActionMessages::START_GENERATE_LOAD) {
				GenerateLoadTriggerMessage const * message = (GenerateLoadTriggerMessage const *)packet->data;
				generateLoadTarget = message->target;
				generateLoadPayloadSize = message->size;
				generateLoadMessagesLeft = message->amount;
				generateLoadTimeBetweenMessagesDs = message->timeBetweenMessagesDs;
				generateLoadRequestHandle = packet->requestHandle;

				logt("NODE", "Generating load. Target: %u size: %u amount: %u interval: %u requestHandle: %u",
					message->target,
					message->size,
					message->amount,
					message->timeBetweenMessagesDs,
					packet->requestHandle);

				SendModuleActionMessage(
					MessageType::MODULE_ACTION_RESPONSE,
					packetHeader->sender,
					(u8)NodeModuleActionResponseMessages::START_GENERATE_LOAD_RESULT,
					packet->requestHandle,
					nullptr,
					0,
					false
				);
			}

			else if (packet->actionType == (u8)NodeModuleTriggerActionMessages::GENERATE_LOAD_CHUNK) {
				u8 const * payload = packet->data;
				bool payloadCorrect = true;
				const u8 payloadLength = sendData->dataLength - SIZEOF_CONN_PACKET_MODULE;
				for (u32 i = 0; i < payloadLength; i++)
				{
					if (payload[i] != generateLoadMagicNumber)
					{
						payloadCorrect = false;
					}
				}

				logjson("NODE", "{\"type\":\"generate_load_chunk\",\"nodeId\":%d,\"size\":%u,\"payloadCorrect\":%u,\"requestHandle\":%u}" SEP, packetHeader->sender, (u32)payloadLength, (u32)payloadCorrect, (u32)packet->requestHandle);
			}
			

			else if (packet->actionType == (u8)NodeModuleTriggerActionMessages::RESET_NODE)
			{
				NodeModuleResetMessage const * message = (NodeModuleResetMessage const *)packet->data;
				logt("NODE", "Scheduled reboot in %u seconds", message->resetSeconds);
				Reboot(message->resetSeconds*10, RebootReason::REMOTE_RESET);
			}
			else if (packet->actionType == (u8)NodeModuleTriggerActionMessages::EMERGENCY_DISCONNECT)
			{
				EmergencyDisconnectResponseMessage response;
				CheckedMemset(&response, 0, sizeof(response));

				if (GS->cm.freeMeshOutConnections == 0)
				{
					MeshConnectionHandle connToDisconnect;

					//We want to disconnect connections with a low number of connected nodes
					//Therefore we give these a higher chance to get disconnected
					u16 rnd = Utility::GetRandomInteger();
					u32 sum = 0;

					MeshConnections conns = GS->cm.GetMeshConnections(ConnectionDirection::DIRECTION_OUT);

					u16 handshakedConnections = 0;
					for (u32 i = 0; i < conns.count; i++) {
						if (conns.handles[i].IsHandshakeDone()) handshakedConnections++;
					}

					//We try to find a connection that we should disconnect based on probability.
					//Connections with less connectedClusterSize should be preferredly disconnected
					for (u32 i = 0; i < conns.count; i++) {
						MeshConnectionHandle conn = conns.handles[i];
						if (!conn.IsHandshakeDone()) continue;

						//The probability from 0 to UINT16_MAX that this connection will be removed
						//Because our node counts against the clusterSize but is not included in the connectedClusterSizes, we substract 1
						//We also check that we do not have a divide by 0 exception
						u32 removalProbability = (handshakedConnections <= 1 || clusterSize <= 1) ? 1 : ((clusterSize - 1) - conn.GetConnectedClusterSize()) * UINT16_MAX / ((handshakedConnections - 1) * (clusterSize - 1));

						sum += removalProbability;

						//TODO: Maybe we do not want linear probablility but more sth. exponential?

						if (sum > rnd) {
							connToDisconnect = conn;
							break;
						}
					}

					if (connToDisconnect) {
						logt("WARNING", "Emergency disconnect from %u", connToDisconnect.GetPartnerId());
						response.code = EmergencyDisconnectErrorCode::SUCCESS;

						connToDisconnect.DisconnectAndRemove(AppDisconnectReason::EMERGENCY_DISCONNECT);
						GS->logger.logCustomError(CustomErrorTypes::INFO_EMERGENCY_DISCONNECT_SUCCESSFUL, 0);

						//TODO: Blacklist other node for a short time
					}
					else {
						response.code = EmergencyDisconnectErrorCode::CANT_DISCONNECT_ANYBODY;
						GS->logger.logCustomCount(CustomErrorTypes::COUNT_EMERGENCY_CONNECTION_CANT_DISCONNECT_ANYBODY);
						logt("WARNING", "WOULD DISCONNECT NOBODY");
					}
				}
				else
				{
					response.code = EmergencyDisconnectErrorCode::NOT_ALL_CONNECTIONS_USED_UP;
				}

				SendModuleActionMessage(
					MessageType::MODULE_ACTION_RESPONSE,
					packetHeader->sender,
					(u8)NodeModuleActionResponseMessages::EMERGENCY_DISCONNECT_RESULT,
					0,
					(u8*)&response,
					sizeof(response),
					false
				);
			}
			else if (packet->actionType == (u8)NodeModuleTriggerActionMessages::SET_PREFERRED_CONNECTIONS)
			{
				PreferredConnectionMessage const * message = (PreferredConnectionMessage const *)packet->data;
				if (message->amountOfPreferredPartnerIds > Conf::MAX_AMOUNT_PREFERRED_PARTNER_IDS)
				{
					//Packet seems to be malformed!
					SIMEXCEPTION(IllegalArgumentException); //LCOV_EXCL_LINE assertion
					return;
				}

				GS->config.configuration.amountOfPreferredPartnerIds = message->amountOfPreferredPartnerIds;
				GS->config.configuration.preferredConnectionMode = message->preferredConnectionMode;
				for (u16 i = 0; i < message->amountOfPreferredPartnerIds; i++) {
					GS->config.configuration.preferredPartnerIds[i] = message->preferredPartnerIds[i];
				}

				Utility::SaveModuleSettingsToFlashWithId(ModuleId::CONFIG, &(GS->config.configuration), sizeof(Conf::ConfigConfiguration), nullptr, 0, nullptr, 0);

				//Reboot is the savest way to make sure that we reevaluate all the possible connection partners.
				Reboot(SEC_TO_DS(10), RebootReason::PREFERRED_CONNECTIONS);

				SendModuleActionMessage(
					MessageType::MODULE_ACTION_RESPONSE,
					packetHeader->sender,
					(u8)NodeModuleActionResponseMessages::SET_PREFERRED_CONNECTIONS_RESULT,
					0,
					nullptr,
					0,
					false
				);
			}
		}
	}

	if(packetHeader->messageType == MessageType::MODULE_ACTION_RESPONSE){
		connPacketModule const * packet = (connPacketModule const *)packetHeader;
		//Check if our module is meant and we should trigger an action
		if(packet->moduleId == ModuleId::NODE){

			if (packet->actionType == (u8)NodeModuleActionResponseMessages::SET_DISCOVERY_RESULT)
			{
				logjson("NODE", "{\"type\":\"set_discovery_result\",\"nodeId\":%d,\"module\":%d}" SEP, packetHeader->sender, (u32)ModuleId::NODE);
			}
			else if (packet->actionType == (u8)NodeModuleActionResponseMessages::PING)
			{
				logjson("NODE", "{\"type\":\"ping\",\"nodeId\":%d,\"module\":%d,\"requestHandle\":%u}" SEP, packetHeader->sender, (u32)ModuleId::NODE, packet->requestHandle);
			}
			else if (packet->actionType == (u8)NodeModuleActionResponseMessages::START_GENERATE_LOAD_RESULT)
			{
				logjson("NODE", "{\"type\":\"start_generate_load_result\",\"nodeId\":%d,\"requestHandle\":%u}" SEP, packetHeader->sender, packet->requestHandle);
			}
			else if (packet->actionType == (u8)NodeModuleActionResponseMessages::EMERGENCY_DISCONNECT_RESULT)
			{
				EmergencyDisconnectResponseMessage const * msg = (EmergencyDisconnectResponseMessage const *)packet->data;
				if (msg->code == EmergencyDisconnectErrorCode::SUCCESS || msg->code == EmergencyDisconnectErrorCode::NOT_ALL_CONNECTIONS_USED_UP)
				{
					//All fine, we are now able to connect to the partner via a MeshConnection
				}
				else if (msg->code == EmergencyDisconnectErrorCode::CANT_DISCONNECT_ANYBODY)
				{
					GS->logger.logCustomError(CustomErrorTypes::WARN_EMERGENCY_DISCONNECT_PARTNER_COULDNT_DISCONNECT_ANYBODY, 0);
				}
				ResetEmergencyDisconnect();
			}
			else if (packet->actionType == (u8)NodeModuleActionResponseMessages::SET_PREFERRED_CONNECTIONS_RESULT)
			{
				logjson("NODE", "{\"type\":\"set_preferred_connections_result\",\"nodeId\":%d,\"module\":%d}" SEP, packetHeader->sender, (u32)ModuleId::NODE);
			}
		}
	}

	if (packetHeader->messageType == MessageType::TIME_SYNC) {
		TimeSyncHeader const * packet = (TimeSyncHeader const *)packetHeader;
		if (packet->type == TimeSyncType::INITIAL)
		{
			TimeSyncInitial const * packet = (TimeSyncInitial const *)packetHeader;
			logt("TSYNC", "Received initial! NodeId: %u, Partner: %u", (u32)GS->node.configuration.nodeId, (u32)packet->header.header.sender);
			GS->timeManager.SetTime(*packet);

			TimeSyncInitialReply reply;
			CheckedMemset(&reply, 0, sizeof(TimeSyncInitialReply));
			reply.header.header.messageType = MessageType::TIME_SYNC;
			reply.header.header.receiver = packet->header.header.sender;
			reply.header.header.sender = packet->header.header.receiver;
			reply.header.type = TimeSyncType::INITIAL_REPLY;

			GS->cm.SendMeshMessage(
				(u8*)&reply,
				sizeof(TimeSyncInitialReply),
				DeliveryPriority::LOW
				);
		}
		if (packet->type == TimeSyncType::INITIAL_REPLY)
		{
			TimeSyncInitialReply const * packet = (TimeSyncInitialReply const *)packetHeader;
			logt("TSYNC", "Received initial reply! NodeId: %u, Partner: %u", (u32)GS->node.configuration.nodeId, (u32)packet->header.header.sender);
			GS->cm.TimeSyncInitialReplyReceivedHandler(*packet);
		}
		if (packet->type == TimeSyncType::CORRECTION)
		{
			TimeSyncCorrection const * packet = (TimeSyncCorrection const *)packetHeader;
			logt("TSYNC", "Received correction! NodeId: %u, Partner: %u", (u32)GS->node.configuration.nodeId, (u32)packet->header.header.sender);
			GS->timeManager.AddCorrection(packet->correctionTicks);

			TimeSyncCorrectionReply reply;
			CheckedMemset(&reply, 0, sizeof(TimeSyncCorrectionReply));
			reply.header.header.messageType = MessageType::TIME_SYNC;
			reply.header.header.receiver = packet->header.header.sender;
			reply.header.header.sender = packet->header.header.receiver;
			reply.header.type = TimeSyncType::CORRECTION_REPLY;

			GS->cm.SendMeshMessage(
				(u8*)&reply,
				sizeof(TimeSyncCorrectionReply),
				DeliveryPriority::LOW
				);
		}
		if (packet->type == TimeSyncType::CORRECTION_REPLY)
		{
			TimeSyncCorrectionReply const * packet = (TimeSyncCorrectionReply const *)packetHeader;
			logt("TSYNC", "Received correction reply! NodeId: %u, Partner: %u", (u32)GS->node.configuration.nodeId, (u32)packet->header.header.sender);
			GS->cm.TimeSyncCorrectionReplyReceivedHandler(*packet);
		}
	}

	if (packetHeader->messageType == MessageType::MODULE_RAW_DATA) {
		RawDataHeader const * packet = (RawDataHeader const *)packetHeader;
		//Check if our module is meant
		if (packet->moduleId == moduleId) {
			const RawDataActionType actionType = (RawDataActionType)packet->actionType;
			if (actionType == RawDataActionType::START && sendData->dataLength >= sizeof(RawDataStart))
			{
				RawDataStart packet = *(RawDataStart const *)packetHeader;

				logjson("DEBUG",
					"{"
						"\"nodeId\":%u,"
						"\"type\":\"raw_data_start\","
						"\"module\":%u,"
						"\"numChunks\":%u,"
						"\"protocol\":%u,"
						"\"fmKeyId\":%u,"
						"\"requestHandle\":%u"
					"}" SEP,
					packet.header.connHeader.sender,
					(u32)moduleId,
					packet.numChunks,
					packet.protocolId,
					packet.fmKeyId,
					packet.header.requestHandle
				);
			}
			else if (actionType == RawDataActionType::START_RECEIVED && sendData->dataLength >= sizeof(RawDataStartReceived))
			{
				RawDataStartReceived packet = *(RawDataStartReceived const *)packetHeader;

				logjson("DEBUG",
					"{"
						"\"nodeId\":%u,"
						"\"type\":\"raw_data_start_received\","
						"\"module\":%u,"
						"\"requestHandle\":%u"
					"}" SEP,
					packet.header.connHeader.sender,
					(u32)moduleId,
					packet.header.requestHandle
				);
			}
			else if (actionType == RawDataActionType::ERROR_T && sendData->dataLength >= sizeof(RawDataError))
			{
				const RawDataError* packet = (RawDataError const *)packetHeader;
				logjson("DEBUG",
					"{"
						"\"nodeId\":%u,"
						"\"type\":\"raw_data_error\","
						"\"module\":%u,"
						"\"error\":%u,"
						"\"destination\":%u,"
						"\"requestHandle\":%u"
					"}" SEP,
					packet->header.connHeader.sender,
					(u32)moduleId,
					(u32)packet->type,
					(u32)packet->destination,
					(u32)packet->header.requestHandle
				);
			}
			else if (actionType == RawDataActionType::CHUNK)
			{
				RawDataChunk const * packet = (RawDataChunk const *)packetHeader;
				if (CHECK_MSG_SIZE(packet, packet->payload, 1, sendData->dataLength))
				{
					const u32 payloadLength = sendData->dataLength - sizeof(RawDataChunk) + 1;
					char payload[250];
					if (payloadLength * 4 / 3 >= sizeof(payload) - 1) {
						SIMEXCEPTION(BufferTooSmallException); //LCOV_EXCL_LINE assertion
					}
					Logger::convertBufferToBase64String(packet->payload, payloadLength, payload, sizeof(payload));

					logjson("DEBUG",
						"{"
							"\"nodeId\":%u,"
							"\"type\":\"raw_data_chunk\","
							"\"module\":%u,"
							"\"chunkId\":%u,"
							"\"payload\":\"%s\","
							"\"requestHandle\":%u"
						"}" SEP,
						packet->header.connHeader.sender,
						(u32)moduleId,
						packet->chunkId,
						payload,
						packet->header.requestHandle
					);
				}
				else
				{
					SIMEXCEPTION(PaketTooSmallException);  //LCOV_EXCL_LINE assertion
				}
			}
			else if (actionType == RawDataActionType::REPORT && sendData->dataLength >= sizeof(RawDataReport))
			{
				RawDataReport const * packet = (RawDataReport const *)packetHeader;

				char missingsBuffer[200] = "[";
				bool successfulTransmission = true;
				for (u32 i = 0; i < sizeof(packet->missings) / sizeof(packet->missings[0]); i++)
				{
					if (packet->missings[i] != 0)
					{
						char singleMissingBuffer[50];
						snprintf(singleMissingBuffer, sizeof(singleMissingBuffer), "%u", packet->missings[i]);

						if (!successfulTransmission) 
						{
							strcat(missingsBuffer, ",");
						}
						strcat(missingsBuffer, singleMissingBuffer);

						successfulTransmission = false;
					}
				}

				strcat(missingsBuffer, "]");


				logjson("DEBUG",
					"{"
						"\"nodeId\":%u,"
						"\"type\":\"raw_data_report\","
						"\"module\":%u,"
						"\"missing\":%s,"
						"\"requestHandle\":%u"
					"}" SEP,
					packet->header.connHeader.sender,
					(u32)moduleId,
					missingsBuffer,
					packet->header.requestHandle
				);
			}
			else
			{
				SIMEXCEPTION(GotUnsupportedActionTypeException); //LCOV_EXCL_LINE assertion
			}
		}
	}
	else if (packetHeader->messageType == MessageType::MODULE_RAW_DATA_LIGHT)
	{
		RawDataLight const * packet = (RawDataLight const *)packetHeader;
		if (CHECK_MSG_SIZE(packet, packet->payload, 1, sendData->dataLength))
		{
			const u32 payloadLength = sendData->dataLength - sizeof(RawDataLight) + 1;
			char payload[250];
			Logger::convertBufferToBase64String(packet->payload, payloadLength, payload, sizeof(payload));

			logjson("DEBUG",
				"{"
					"\"nodeId\":%u,"
					"\"type\":\"raw_data_light\","
					"\"module\":%u,"
					"\"protocol\":%u,"
					"\"payload\":\"%s\","
					"\"requestHandle\":%u"
				"}" SEP,
				packet->connHeader.sender,
				(u32)moduleId,
				(u32)packet->protocolId,
				payload,
				packet->requestHandle
			);
		}
		else
		{
			SIMEXCEPTION(PaketTooSmallException); //LCOV_EXCL_LINE assertion
		}
	}
	else if (packetHeader->messageType == MessageType::CAPABILITY)
	{
		if (sendData->dataLength >= sizeof(CapabilityHeader)) 
		{
			CapabilityHeader const * header = (CapabilityHeader const *)packetHeader;
			if (header->actionType == CapabilityActionType::REQUESTED)
			{
				isSendingCapabilities = true;
				firstCallForCurrentCapabilityModule = true;
				timeSinceLastCapabilitySentDs = TIME_BETWEEN_CAPABILITY_SENDINGS_DS; //Immediately send first capability uppon next timerEventHandler call.
				capabilityRetrieverModuleIndex = 0;
				capabilityRetrieverLocal = 0;
				capabilityRetrieverGlobal = 0;

				logt("NODE", "Capabilities are requested");
			}
			else if (header->actionType == CapabilityActionType::ENTRY)
			{
				if (sendData->dataLength >= sizeof(CapabilityEntryMessage))
				{
					CapabilityEntryMessage const * message = (CapabilityEntryMessage const *)packetHeader;

					char buffer[sizeof(message->entry.modelName) + 1]; //Buffer to make sure we have a terminating zero.

					//Several logjson calls to go easy on stack size
					logjson_partial("NODE", "{");
					logjson_partial("NODE",		"\"nodeId\":%u,", message->header.header.sender);
					logjson_partial("NODE",		"\"type\":\"capability_entry\",");
					logjson_partial("NODE",		"\"index\":%u,", message->index);
					logjson_partial("NODE",		"\"capabilityType\":%u,", (u32)message->entry.type);
					CheckedMemcpy(buffer, message->entry.manufacturer, sizeof(message->entry.manufacturer));
					buffer[sizeof(message->entry.manufacturer)] = '\0';
					logjson_partial("NODE",		"\"manufacturer\":\"%s\",", buffer);
					CheckedMemcpy(buffer, message->entry.modelName, sizeof(message->entry.modelName));
					buffer[sizeof(message->entry.modelName)] = '\0';
					logjson_partial("NODE",		"\"model\":\"%s\",", buffer);
					CheckedMemcpy(buffer, message->entry.revision, sizeof(message->entry.revision));
					buffer[sizeof(message->entry.revision)] = '\0';
					logjson_partial("NODE",		"\"revision\":\"%s\"", buffer);
					logjson("NODE", "}" SEP);
				}
				else
				{
					SIMEXCEPTION(PaketTooSmallException); //LCOV_EXCL_LINE assertion
				}
			}
			else if (header->actionType == CapabilityActionType::END)
			{
				if (sendData->dataLength >= sizeof(CapabilityEndMessage))
				{
					CapabilityEndMessage const * message = (CapabilityEndMessage const *)packetHeader;
					logjson("NODE", 
						"{"
							"\"nodeId\":%u,"
							"\"type\":\"capability_end\","
							"\"amount\":%u"
						"}" SEP,
						message->header.header.sender,
						message->amountOfCapabilities
					);
				}
				else
				{
					SIMEXCEPTION(PaketTooSmallException); //LCOV_EXCL_LINE assertion
				}
			}
		}
		else
		{
			SIMEXCEPTION(PaketTooSmallException); //LCOV_EXCL_LINE assertion
		}
	}

	else if (packetHeader->messageType == MessageType::COMPONENT_SENSE)
	{
		connPacketComponentMessage const * packet = (connPacketComponentMessage const *)packetHeader;

		char payload[50];
		u8 payloadLength = sendData->dataLength - sizeof(packet->componentHeader);
		Logger::convertBufferToBase64String(packet->payload,  payloadLength, payload, sizeof(payload));
		logjson("NODE",
			"{"
				"\"nodeId\":%u,"
				"\"type\":\"component_sense\","
				"\"module\":%u,"
				"\"requestHandle\":%u,"
				"\"actionType\":%u,"
				"\"component\":\"0x%04X\","
				"\"register\":\"0x%04X\","
				"\"payload\":\"%s\""
			"}" SEP,
		packet->componentHeader.header.sender,
		(u32)packet->componentHeader.moduleId,
		packet->componentHeader.requestHandle,
		packet->componentHeader.actionType,
		packet->componentHeader.component,
		packet->componentHeader.registerAddress,
		payload);

	}

	else if (packetHeader->messageType == MessageType::COMPONENT_ACT)
	{
		connPacketComponentMessage const * packet = (connPacketComponentMessage const *)packetHeader;

		char payload[50];
		u8 payloadLength = sendData->dataLength - sizeof(packet->componentHeader);
		Logger::convertBufferToHexString(packet->payload,  payloadLength, payload, sizeof(payload));
		logt("NODE","component_act payload = %s",payload);

	}
}
//Processes incoming CLUSTER_INFO_UPDATE packets
/*
 #########################################################################################################
 ### Advertising and Receiving advertisements
 #########################################################################################################
 */
#define ________________ADVERTISING___________________
                                                                                    
//Start to broadcast our own clusterInfo, set ackID if we want to have an ack or an ack response
void Node::UpdateJoinMePacket() const
{
	if (configuration.networkId == 0) return;
	if (meshAdvJobHandle == nullptr) return;
	if (GET_DEVICE_TYPE() == DeviceType::ASSET) return;

	SetTerminalTitle();

	u8* buffer = meshAdvJobHandle->advData;

	advPacketHeader* advPacket = (advPacketHeader*)buffer;
	advPacket->flags.len = SIZEOF_ADV_STRUCTURE_FLAGS-1; //minus length field itself
	advPacket->flags.type = (u8)BleGapAdType::TYPE_FLAGS;
	advPacket->flags.flags = FH_BLE_GAP_ADV_FLAG_LE_GENERAL_DISC_MODE | FH_BLE_GAP_ADV_FLAG_BR_EDR_NOT_SUPPORTED;

	advPacket->manufacturer.len = (SIZEOF_ADV_STRUCTURE_MANUFACTURER + SIZEOF_ADV_PACKET_STUFF_AFTER_MANUFACTURER + SIZEOF_ADV_PACKET_PAYLOAD_JOIN_ME_V0) - 1;
	advPacket->manufacturer.type = (u8)BleGapAdType::TYPE_MANUFACTURER_SPECIFIC_DATA;
	advPacket->manufacturer.companyIdentifier = MESH_COMPANY_IDENTIFIER;

	advPacket->meshIdentifier = MESH_IDENTIFIER;
	advPacket->networkId = configuration.networkId;
	advPacket->messageType = ServiceDataMessageType::JOIN_ME_V0;

	//Build a JOIN_ME packet and set it in the advertisement data
	advPacketPayloadJoinMeV0* packet = (advPacketPayloadJoinMeV0*)(buffer+SIZEOF_ADV_PACKET_HEADER);
	packet->sender = configuration.nodeId;
	packet->clusterId = this->clusterId;
	packet->clusterSize = this->clusterSize;
	packet->freeMeshInConnections = GS->cm.freeMeshInConnections;
	packet->freeMeshOutConnections = GS->cm.freeMeshOutConnections;

	//A leaf only has one free in connection
	if(GET_DEVICE_TYPE() == DeviceType::LEAF){
		if(GS->cm.freeMeshInConnections > 0) packet->freeMeshInConnections = 1;
		packet->freeMeshOutConnections = 0;
	}

	StatusReporterModule* statusMod = (StatusReporterModule*)this->GetModuleById(ModuleId::STATUS_REPORTER_MODULE);
	if(statusMod != nullptr){
		packet->batteryRuntime = statusMod->GetBatteryVoltage();
	} else {
		packet->batteryRuntime = 0;
	}

	packet->txPower = Conf::defaultDBmTX;
	packet->deviceType = GET_DEVICE_TYPE();
	packet->hopsToSink = GS->cm.GetMeshHopsToShortestSink(nullptr);
	packet->meshWriteHandle = meshService.sendMessageCharacteristicHandle.valueHandle;

	//We only use the concept of ackIds if we only use one mesh inConnection
	//Otherwhise, we do not need to use it as a partner can use our free inConnection
	if (GS->config.meshMaxInConnections == 1) {
		if (currentAckId != 0)
		{
			packet->ackField = currentAckId;

		}
		else {
			packet->ackField = 0;
		}
	}

	meshAdvJobHandle->advDataLength = SIZEOF_ADV_PACKET_HEADER + SIZEOF_ADV_PACKET_PAYLOAD_JOIN_ME_V0;

	logt("JOIN", "JOIN_ME updated clusterId:%x, clusterSize:%d, freeIn:%u, freeOut:%u, handle:%u, ack:%u", packet->clusterId, packet->clusterSize, packet->freeMeshInConnections, packet->freeMeshOutConnections, packet->meshWriteHandle, packet->ackField);

	logjson("SIM", "{\"type\":\"update_joinme\",\"clusterId\":%u,\"clusterSize\":%d}" SEP, clusterId, clusterSize);

	//Stop advertising if we are already connected as a leaf. Necessary for EoModule
	if(GET_DEVICE_TYPE() == DeviceType::LEAF && GS->cm.freeMeshInConnections == 0){
		meshAdvJobHandle->slots = 0;
	} else if(GET_DEVICE_TYPE() == DeviceType::LEAF){
		meshAdvJobHandle->slots = 5;
	}

	GS->advertisingController.RefreshJob(meshAdvJobHandle);
}


//This can be called to temporarily broadcast the join_me packet very frequently, e.g. if we want to reconnect
void Node::StartFastJoinMeAdvertising()
{
	//Immediately start a fast advertisement to speed up the reconnection
	AdvJob job = {
		AdvJobTypes::IMMEDIATE,
		10, //10 Slot * timer interval
		0, //Delay
		MSEC_TO_UNITS(20, CONFIG_UNIT_0_625_MS), //AdvInterval
		0, //AdvChannel
		0, //CurrentSlots
		0, //CurrentDelay
		FruityHal::BleGapAdvType::ADV_IND, //Advertising Mode
		{}, //AdvData
		3, //AdvDataLength
		{0}, //ScanData
		0 //ScanDataLength
	};

	//Copy the content of the current join_me packet
	CheckedMemcpy(job.advData, meshAdvJobHandle->advData, ADV_PACKET_MAX_SIZE);
	job.advDataLength = meshAdvJobHandle->advDataLength;

	//Add the job, it will be removed after it has no more slots left
	GS->advertisingController.AddJob(job);
}

//STEP 3: After collecting all available clusters, we want to connect to the best cluster that is available
//If the other clusters were not good and we have something better, we advertise it.
Node::DecisionStruct Node::DetermineBestClusterAvailable(void)
{
	DecisionStruct result = { DecisionResult::NO_NODES_FOUND, 0, 0 };

	joinMeBufferPacket* bestClusterAsMaster = DetermineBestClusterAsMaster();

	//If we still do not have a freeOutConnection, we have no viable cluster to connect to
	if (GS->cm.freeMeshOutConnections > 0)
	{
		//Now, if we want to be a master in the connection, we simply answer the ad packet that
		//informs us about that cluster
		if (bestClusterAsMaster != nullptr)
		{
			currentAckId = 0;

			FruityHal::BleGapAddr address = bestClusterAsMaster->addr;

			//Choose a different connection interval for leaf nodes
			u16 connectionIv = Conf::getInstance().meshMinConnectionInterval;
			if(bestClusterAsMaster->payload.deviceType == DeviceType::LEAF){
				connectionIv = MSEC_TO_UNITS(90, CONFIG_UNIT_1_25_MS);
			}

			ErrorType err = GS->cm.ConnectAsMaster(bestClusterAsMaster->payload.sender, &address, bestClusterAsMaster->payload.meshWriteHandle, connectionIv);

			//Note the time that we tried to connect to this node so that we can blacklist it for some time if it does not work
			if (err == ErrorType::SUCCESS) {
				bestClusterAsMaster->lastConnectAttemptDs = GS->appTimerDs;
				if(bestClusterAsMaster->attemptsToConnect <= 20) bestClusterAsMaster->attemptsToConnect++;
			}

			result.result = DecisionResult::CONNECT_AS_MASTER;
			result.preferredPartner = bestClusterAsMaster->payload.sender;
			return result;
		}
	}

	//If no good cluster could be found (all are bigger than mine)
	//Find the best cluster that should connect to us (we as slave)
	currentAckId = 0;
	joinMeBufferPacket* bestClusterAsSlave = DetermineBestClusterAsSlave();

	//Set our ack field to the best cluster that we want to be a part of
	if (bestClusterAsSlave != nullptr)
	{
		currentAckId = bestClusterAsSlave->payload.clusterId;

		logt("DECISION", "Other clusters are bigger, we are going to be a slave of %u", currentAckId);

		//For nodes with only 1 meshInConnection, we must disconnect from a cluster if a bigger cluster is found nearby
		if (GS->config.meshMaxInConnections == 1) {

			//Check if we have a recently established connection and do not disconnect if yes bofore the handshake has not timed out
			bool freshConnectionAvailable = false;
			BaseConnections conns = GS->cm.GetBaseConnections(ConnectionDirection::INVALID);
			for (u32 i = 0; i < conns.count; i++) {
				BaseConnectionHandle conn = conns.handles[i];
				if (conn) {
					if (conn.GetCreationTimeDs() + Conf::meshHandshakeTimeoutDs > GS->appTimerDs) {
						freshConnectionAvailable = true;
						break;
					}
				}
			}
			//Only if we are not currently doing a handshake and if we do not have a freeInConnection
			if (!freshConnectionAvailable && GS->cm.freeMeshInConnections == 0) {
				if (
					//Check if we have either different clusterSizes or if similar, only disconnect randomly
					//to prevent recurrent situations where two nodes will always disconnect at the same time
					clusterSize != bestClusterAsSlave->payload.clusterSize
					|| Utility::GetRandomInteger() < UINT32_MAX / 4
				) {
					GS->cm.ForceDisconnectOtherMeshConnections(nullptr, AppDisconnectReason::SHOULD_WAIT_AS_SLAVE);

					clusterSize = 1;
					clusterId = GenerateClusterID();
				}
			}
		}

		UpdateJoinMePacket();

		result.result = DecisionResult::CONNECT_AS_SLAVE;
		result.preferredPartner = bestClusterAsSlave->payload.sender;
		return result;
	}

	logt("DECISION", "no cluster found");

	result.result = DecisionResult::NO_NODES_FOUND;
	return result;
}

u32 Node::ModifyScoreBasedOnPreferredPartners(u32 score, NodeId partner) const
{
	if (score > 0 && !IsPreferredConnection(partner))
	{
		if (GS->config.configuration.preferredConnectionMode == PreferredConnectionMode::PENALTY)
		{
			score /= 10;
			if (score < 1) score = 1; //If the mode is set to penalty, we don't want to ignore any possible cluster.
		}
		else if (GS->config.configuration.preferredConnectionMode == PreferredConnectionMode::IGNORED)
		{
			score = 0;
		}
		else
		{
			//This PreferredConnectionMode is not implemented.
			SIMEXCEPTION(IllegalStateException); //LCOV_EXCL_LINE assertion
		}
	}

	return score;
}

joinMeBufferPacket * Node::DetermineBestCluster(u32(Node::*clusterRatingFunction)(const joinMeBufferPacket &packet) const)
{
	u32 bestScore = 0;
	joinMeBufferPacket* bestCluster = nullptr;

	for (u32 i = 0; i < joinMePackets.size(); i++)
	{
		joinMeBufferPacket* packet = &joinMePackets[i];
		if (packet->payload.sender == 0) continue;

		u32 score = (this->*clusterRatingFunction)(*packet);
		if (score > bestScore)
		{
			bestScore = score;
			bestCluster = packet;
		}
	}
	return bestCluster;
}

joinMeBufferPacket* Node::DetermineBestClusterAsSlave()
{
	return DetermineBestCluster(&Node::CalculateClusterScoreAsSlave);
}

joinMeBufferPacket* Node::DetermineBestClusterAsMaster()
{
	return DetermineBestCluster(&Node::CalculateClusterScoreAsMaster);
}

//Calculates the score for a cluster
//Connect to big clusters but big clusters must connect nodes that are not able 
u32 Node::CalculateClusterScoreAsMaster(const joinMeBufferPacket& packet) const
{
	//If the packet is too old, filter it out
	if (GS->appTimerDs - packet.receivedTimeDs > MAX_JOIN_ME_PACKET_AGE_DS) return 0;

	//If we are already connected to that cluster, the score is 0
	if (packet.payload.clusterId == this->clusterId) return 0;

	//If there are zero free in connections, we cannot connect as master
	if (packet.payload.freeMeshInConnections == 0) return 0;

	//If the other node wants to connect as a slave to another cluster, do not connect
	if (packet.payload.ackField != 0 && packet.payload.ackField != this->clusterId) return 0;

	//If the other cluster is bigger, we cannot connect as master
	if (packet.payload.clusterSize > this->clusterSize) return 0;

	//Check if we recently tried to connect to him and blacklist him for a short amount of time
	if (
		packet.lastConnectAttemptDs != 0
		&& packet.attemptsToConnect > connectAttemptsBeforeBlacklisting
		&& packet.lastConnectAttemptDs + SEC_TO_DS(1) * packet.attemptsToConnect > GS->appTimerDs) {
		SIMSTATCOUNT("tempBlacklist");
		logt("NODE", "temporarily blacklisting node %u, attempts: %u", packet.payload.sender, packet.attemptsToConnect);
		return 0;
	}

	//Do not connect if we are already connected to that partner
	if (GS->cm.GetMeshConnectionToPartner(packet.payload.sender)) return 0;

	//Connection should have a minimum of stability
	if(packet.rssi < STABLE_CONNECTION_RSSI_THRESHOLD) return 0;

	u32 rssiScore = 100 + packet.rssi;

	//If we are a leaf node, we must not connect to anybody
	if(GET_DEVICE_TYPE() == DeviceType::LEAF) return 0;

	//Free in connections are best, free out connections are good as well
	//TODO: RSSI should be factored into the score as well, maybe battery runtime, device type, etc...
	u32 score = (u32)(packet.payload.freeMeshInConnections) * 10000 + (u32)(packet.payload.freeMeshOutConnections) * 100 + rssiScore;

	return ModifyScoreBasedOnPreferredPartners(score, packet.payload.sender);
}

//If there are only bigger clusters around, we want to find the best
//And set its id in our ack field
u32 Node::CalculateClusterScoreAsSlave(const joinMeBufferPacket& packet) const
{
	//If the packet is too old, filter it out
	if (GS->appTimerDs - packet.receivedTimeDs > MAX_JOIN_ME_PACKET_AGE_DS) return 0;

	//If we are already connected to that cluster, the score is 0
	if (packet.payload.clusterId == this->clusterId) return 0;

	//Do not check for freeOut == 0 as the partner will probably free up a conneciton for us and we should be ready

	//We will only be a slave of a bigger or equal cluster
	if (packet.payload.clusterSize < this->clusterSize) return 0;

	//Connection should have a minimum of stability
	if(packet.rssi < STABLE_CONNECTION_RSSI_THRESHOLD) return 0;

	u32 rssiScore = 100 + packet.rssi;

	//Choose the one with the biggest cluster size, if there are more, prefer the most outConnections
	u32 score = (u32)(packet.payload.clusterSize) * 10000 + (u32)(packet.payload.freeMeshOutConnections) * 100 + rssiScore;

	return ModifyScoreBasedOnPreferredPartners(score, packet.payload.sender);
}

bool Node::DoesBiggerKnownClusterExist()
{
	return DetermineBestClusterAsSlave() != nullptr;
}

void Node::ResetEmergencyDisconnect()
{
	emergencyDisconnectTimerDs = 0;
	if (emergencyDisconnectValidationConnectionUniqueId)
	{
		emergencyDisconnectValidationConnectionUniqueId.DisconnectAndRemove(AppDisconnectReason::EMERGENCY_DISCONNECT_RESET);
		emergencyDisconnectValidationConnectionUniqueId = MeshAccessConnectionHandle();
	}
}

//All advertisement packets are received here if they are valid
void Node::GapAdvertisementMessageHandler(const FruityHal::GapAdvertisementReportEvent& advertisementReportEvent)
{
	if (GET_DEVICE_TYPE() == DeviceType::ASSET) return;

	const u8* data = advertisementReportEvent.getData();
	u16 dataLength = advertisementReportEvent.getDataLength();

	const advPacketHeader* packetHeader = (const advPacketHeader*) data;

	if (packetHeader->messageType == ServiceDataMessageType::JOIN_ME_V0)
	{
		if (dataLength == SIZEOF_ADV_PACKET_JOIN_ME)
		{
			GS->logger.logCustomCount(CustomErrorTypes::COUNT_JOIN_ME_RECEIVED);

			const advPacketJoinMeV0* packet = (const advPacketJoinMeV0*) data;

			logt("DISCOVERY", "JOIN_ME: sender:%u, clusterId:%x, clusterSize:%d, freeIn:%u, freeOut:%u, ack:%u", packet->payload.sender, packet->payload.clusterId, packet->payload.clusterSize, packet->payload.freeMeshInConnections, packet->payload.freeMeshOutConnections, packet->payload.ackField);

			//Look through the buffer and determine a space where we can put the packet in
			joinMeBufferPacket* targetBuffer = findTargetBuffer(packet);

			//Now, we have the space for our packet and we fill it with the latest information
			if (targetBuffer != nullptr)
			{
				CheckedMemcpy(targetBuffer->addr.addr, advertisementReportEvent.getPeerAddr(), FH_BLE_GAP_ADDR_LEN);
				targetBuffer->addr.addr_type = advertisementReportEvent.getPeerAddrType();
				targetBuffer->advType = advertisementReportEvent.isConnectable() ? FruityHal::BleGapAdvType::ADV_IND : FruityHal::BleGapAdvType::ADV_NONCONN_IND;
				targetBuffer->rssi = advertisementReportEvent.getRssi();
				targetBuffer->receivedTimeDs = GS->appTimerDs;

				targetBuffer->payload = packet->payload;
			}
		}
	}

}

joinMeBufferPacket* Node::findTargetBuffer(const advPacketJoinMeV0* packet)
{
	joinMeBufferPacket* targetBuffer = nullptr;

	//First, look if a packet from this node is already in the buffer, if yes, we use this space
	for (u32 i = 0; i < joinMePackets.size(); i++)
	{
		targetBuffer = &joinMePackets[i];

		if (packet->payload.sender == targetBuffer->payload.sender)
		{
			logt("DISCOVERY", "Updated old buffer packet");
			return targetBuffer;
		}
	}
	targetBuffer = nullptr;

	//Next, we look if there's an empty space
	for (u32 i = 0; i < joinMePackets.size(); i++)
	{
		targetBuffer = &(joinMePackets[i]);

		if(targetBuffer->payload.sender == 0)
		{
			logt("DISCOVERY", "Used empty space");
			KeepHighDiscoveryActive();
			return targetBuffer;
		}
	}
	targetBuffer = nullptr;

	//Next, we can overwrite the oldest packet that we saved from our own cluster
	u32 oldestTimestamp = UINT32_MAX;
	for (u32 i = 0; i < joinMePackets.size(); i++)
	{
		joinMeBufferPacket* tmpPacket = &joinMePackets[i];

		if(tmpPacket->payload.clusterId == clusterId && tmpPacket->receivedTimeDs < oldestTimestamp){
			oldestTimestamp = tmpPacket->receivedTimeDs;
			targetBuffer = tmpPacket;
		}
	}

	if(targetBuffer != nullptr){
		logt("DISCOVERY", "Overwrote one from our own cluster");
		return targetBuffer;
	}

	//If there's still no space, we overwrite the oldest packet that we received, this will not fail
	//TODO: maybe do not use oldest one but worst candidate?? Use clusterScore on all packets to find the least interesting
	u32 minScore = UINT32_MAX;
	for (u32 i = 0; i < joinMePackets.size(); i++)
	{
		joinMeBufferPacket* tmpPacket = &joinMePackets[i];

		u32 score = 0;
		if (packet->payload.clusterSize >= clusterSize) {
			score = CalculateClusterScoreAsMaster(*tmpPacket);
		}
		else {
			score = CalculateClusterScoreAsSlave(*tmpPacket);
		}

		if(score < minScore){
			minScore = score;
			targetBuffer = tmpPacket;
		}
	}

	logt("DISCOVERY", "Overwrote worst packet from different cluster");
	return targetBuffer;
}

/*
 #########################################################################################################
 ### Advertising and Receiving advertisements
 #########################################################################################################
 */
#define ________________STATES___________________

void Node::ChangeState(DiscoveryState newState)
{
	if (currentDiscoveryState == newState || stateMachineDisabled || GET_DEVICE_TYPE() == DeviceType::ASSET){
		currentStateTimeoutDs = (currentDiscoveryState == newState) ? SEC_TO_DS((u32)Conf::getInstance().highToLowDiscoveryTimeSec) : currentStateTimeoutDs;
		return;
	}

	currentDiscoveryState = newState;

	if (newState == DiscoveryState::HIGH)
	{
		logt("STATES", "-- DISCOVERY HIGH --");

		//Reset no nodes found counter
		noNodesFoundCounter = 0;

		currentStateTimeoutDs = SEC_TO_DS((u32)Conf::getInstance().highToLowDiscoveryTimeSec);
		nextDiscoveryState = Conf::getInstance().highToLowDiscoveryTimeSec == 0 ? DiscoveryState::INVALID : DiscoveryState::LOW;

		//Reconfigure the advertising and scanning jobs
		if (meshAdvJobHandle != nullptr){
			meshAdvJobHandle->advertisingInterval =	Conf::meshAdvertisingIntervalHigh;
			meshAdvJobHandle->slots = 5;
			GS->advertisingController.RefreshJob(meshAdvJobHandle);
		}

		GS->scanController.UpdateJobPointer(&p_scanJob, ScanState::HIGH, ScanJobState::ACTIVE);
	}
	else if (newState == DiscoveryState::LOW)
	{
		logt("STATES", "-- DISCOVERY LOW --");

		currentStateTimeoutDs = 0;
		nextDiscoveryState = DiscoveryState::INVALID;

		//Reconfigure the advertising and scanning jobs
		if (meshAdvJobHandle != nullptr) {
			meshAdvJobHandle->advertisingInterval = Conf::meshAdvertisingIntervalLow;
			GS->advertisingController.RefreshJob(meshAdvJobHandle);
		}
		ScanJob scanJob = ScanJob();
		scanJob.type = ScanState::LOW;
		scanJob.state = ScanJobState::ACTIVE;
		GS->scanController.RemoveJob(p_scanJob);
		p_scanJob = nullptr;

		p_scanJob = GS->scanController.AddJob(scanJob);
	}
	else if (newState == DiscoveryState::OFF)
	{
		logt("STATES", "-- DISCOVERY OFF --");

		nextDiscoveryState = DiscoveryState::INVALID;

		meshAdvJobHandle->slots = 0;
		GS->advertisingController.RefreshJob(meshAdvJobHandle);

		GS->scanController.RemoveJob(p_scanJob);
		p_scanJob = nullptr;
	}
}

void Node::DisableStateMachine(bool disable)
{
	stateMachineDisabled = disable;
}

void Node::TimerEventHandler(u16 passedTimeDs)
{
	currentStateTimeoutDs -= passedTimeDs;

	//Check if we should switch states because of timeouts
	if (nextDiscoveryState != DiscoveryState::INVALID && currentStateTimeoutDs <= 0)
	{
		//Go to the next state
		ChangeState(nextDiscoveryState);
	}

	if (DoesBiggerKnownClusterExist())
	{
		const u32 emergencyDisconnectTimerBackupDs = emergencyDisconnectTimerDs;
		emergencyDisconnectTimerDs += passedTimeDs;

		//If the emergencyDisconnectTimerTriggerDs was surpassed in this TimerEventHandler
		if(    emergencyDisconnectTimerBackupDs <  emergencyDisconnectTimerTriggerDs
			&& emergencyDisconnectTimerDs       >= emergencyDisconnectTimerTriggerDs)
		{
			joinMeBufferPacket* bestCluster = DetermineBestClusterAsSlave();
			emergencyDisconnectValidationConnectionUniqueId = MeshAccessConnectionHandle(MeshAccessConnection::ConnectAsMaster(&bestCluster->addr, 10, 10, FmKeyId::NETWORK, nullptr, MeshAccessTunnelType::PEER_TO_PEER));
			//If a connection wasn't possible to establish
			if (!emergencyDisconnectValidationConnectionUniqueId.IsValid())
			{
				//We reset all the emergency disconnect values and try again after emergencyDisconnectTimerTriggerDs
				ResetEmergencyDisconnect();
				GS->logger.logCustomError(CustomErrorTypes::WARN_COULD_NOT_CREATE_EMERGENCY_DISCONNECT_VALIDATION_CONNECTION, bestCluster->payload.clusterId);
			}
		}
		else if (emergencyDisconnectTimerDs >= emergencyDisconnectTimerTriggerDs)
		{
			if (emergencyDisconnectValidationConnectionUniqueId)
			{
				if (emergencyDisconnectValidationConnectionUniqueId.GetConnectionState() == ConnectionState::HANDSHAKE_DONE)
				{
					SendModuleActionMessage(
						MessageType::MODULE_TRIGGER_ACTION,
						emergencyDisconnectValidationConnectionUniqueId.GetVirtualPartnerId(),
						(u8)NodeModuleTriggerActionMessages::EMERGENCY_DISCONNECT,
						0,
						nullptr,
						0,
						false
					);
				}
			}
			else
			{
				ResetEmergencyDisconnect();
				//This can happen in very rare conditions where several nodes enter the emergency state at the same time
				//and report their emergency to the same node.
				GS->logger.logCustomError(CustomErrorTypes::WARN_UNEXPECTED_REMOVAL_OF_EMERGENCY_DISCONNECT_VALIDATION_CONNECTION, 0);
			}
		}
	}
	else
	{
		ResetEmergencyDisconnect();
	}

	//Count the nodes that are a good choice for connecting
	//TODO: We could use this snippet to connect immediately after enought nodes were collected
//	u8 numGoodNodesInBuffer = 0;
//	for (int i = 0; i < joinMePacketBuffer->_numElements; i++)
//	{
//		joinMeBufferPacket* packet = (joinMeBufferPacket*) joinMePacketBuffer->PeekItemAt(i);
//		u32 score = CalculateClusterScoreAsMaster(packet);
//		if (score > 0){
//			numGoodNodesInBuffer++;
//		}
//	}
//
//	if(numGoodNodesInBuffer >= Config->numNodesForDecision) ...

	//Check if there is a good cluster but add a random delay 
	if(lastDecisionTimeDs + Conf::maxTimeUntilDecisionDs <= GS->appTimerDs)
	{
		DecisionStruct decision = DetermineBestClusterAvailable();

		if (decision.result == Node::DecisionResult::NO_NODES_FOUND && noNodesFoundCounter < 100){
			noNodesFoundCounter++;
		} else if (decision.result == Node::DecisionResult::CONNECT_AS_MASTER || decision.result == Node::DecisionResult::CONNECT_AS_SLAVE){
			noNodesFoundCounter = 0;
		}

		//Save the last decision time and add a random delay so that two nodes that connect to each other will not repeatedly do so at the same time
		lastDecisionTimeDs = GS->appTimerDs + (Utility::GetRandomInteger() % 2 == 0 ? 1 : 0);

		StatusReporterModule* statusMod = (StatusReporterModule*)GS->node.GetModuleById(ModuleId::STATUS_REPORTER_MODULE);
		if(statusMod != nullptr){
			statusMod->SendLiveReport(LiveReportTypes::DECISION_RESULT, 0, (u8)(decision.result), decision.preferredPartner);
		}
	}

	if((disconnectTimestampDs !=0 && GS->appTimerDs >= disconnectTimestampDs + SEC_TO_DS(TIME_BEFORE_DISCOVERY_MESSAGE_SENT_SEC))&& Conf::getInstance().highToLowDiscoveryTimeSec != 0){
		logt("NODE","High Discovery message being sent after disconnect");
		//Message is broadcasted when connnection is lost to change the state to High Discovery
			u8 discoveryState = (u8)DiscoveryState::HIGH;
			SendModuleActionMessage(
				MessageType::MODULE_TRIGGER_ACTION,
				NODE_ID_BROADCAST,
				(u8)NodeModuleTriggerActionMessages::SET_DISCOVERY,
				0,
				&discoveryState,
				1,
				false
			);

			disconnectTimestampDs = 0;
	}

	//Reboot if a time is set
	if(rebootTimeDs != 0 && rebootTimeDs < GS->appTimerDs){
		logt("NODE", "Resetting!");
		//Do not reboot in safe mode
		*GS->rebootMagicNumberPtr = REBOOT_MAGIC_NUMBER;

		GS->ramRetainStructPtr->crc32 = Utility::CalculateCrc32((u8*)GS->ramRetainStructPtr, sizeof(RamRetainStruct) - 4);
		if (GS->ramRetainStructPtr->rebootReason == RebootReason::DFU){
#ifdef SIM_ENABLED
			cherrySimInstance->currentNode->fakeDfuVersionArmed = true;
#endif
			FruityHal::FeedWatchdog();
		}

		//Disconnect all connections on purpose so that others know the reason and do not reestablish
		GS->cm.ForceDisconnectAllConnections(AppDisconnectReason::REBOOT);
		//We must wait for a short while until the disconnect was done
		FruityHal::DelayMs(500);
		
		FruityHal::SystemReset();
	}


	if (isSendingCapabilities) {
		timeSinceLastCapabilitySentDs += passedTimeDs;
		if (timeSinceLastCapabilitySentDs >= TIME_BETWEEN_CAPABILITY_SENDINGS_DS)
		{
			//Implemented as fixedDelay instead of fixedRate, thus setting the variable to 0 instead of subtracting TIME_BETWEEN_CAPABILITY_SENDINGS_DS
			timeSinceLastCapabilitySentDs = 0;

			alignas(u32) CapabilityEntryMessage messageEntry;
			CheckedMemset(&messageEntry, 0, sizeof(CapabilityEntryMessage));
			messageEntry.header.header.messageType = MessageType::CAPABILITY;
			messageEntry.header.header.receiver = NODE_ID_BROADCAST;	//TODO this SHOULD be NODE_ID_SHORTEST_SINK, however that currently does not reach node 0 in the runner. Bug?
			messageEntry.header.header.sender = configuration.nodeId;
			messageEntry.header.actionType = CapabilityActionType::ENTRY;
			messageEntry.index = capabilityRetrieverGlobal;
			messageEntry.entry = GetNextGlobalCapability();

			if (messageEntry.entry.type == CapabilityEntryType::INVALID)
			{
				alignas(u32) CapabilityEndMessage message;
				CheckedMemset(&message, 0, sizeof(CapabilityEndMessage));
				message.header.header = messageEntry.header.header;
				message.header.actionType = CapabilityActionType::END;
				message.amountOfCapabilities = capabilityRetrieverGlobal;
				GS->cm.SendMeshMessage(
					(u8*)&message,
					sizeof(CapabilityEndMessage),
					DeliveryPriority::LOW
				);
			}
			else if (messageEntry.entry.type == CapabilityEntryType::NOT_READY)
			{
				// If the module wasn't ready yet, we immediately
				// retry it on the next TimerEventHandler call.
				timeSinceLastCapabilitySentDs = TIME_BETWEEN_CAPABILITY_SENDINGS_DS;
			}
			else
			{
				GS->cm.SendMeshMessage(
					(u8*)&messageEntry,
					sizeof(CapabilityEntryMessage),
					DeliveryPriority::LOW
				);
			}
		}
	}

	/*************************/
	/***                   ***/
	/***   GENERATE_LOAD   ***/
	/***                   ***/
	/*************************/
	if (generateLoadMessagesLeft > 0)
	{
		generateLoadTimeSinceLastMessageDs += passedTimeDs;
		while (generateLoadTimeSinceLastMessageDs >= generateLoadTimeBetweenMessagesDs
		    && generateLoadMessagesLeft > 0)
		{
			generateLoadTimeSinceLastMessageDs -= generateLoadTimeBetweenMessagesDs;
			generateLoadMessagesLeft--;

			DYNAMIC_ARRAY(payloadBuffer, generateLoadPayloadSize);
			CheckedMemset(payloadBuffer, generateLoadMagicNumber, generateLoadPayloadSize);

			SendModuleActionMessage(
				MessageType::MODULE_TRIGGER_ACTION,
				generateLoadTarget,
				(u8)NodeModuleTriggerActionMessages::GENERATE_LOAD_CHUNK,
				generateLoadRequestHandle,
				payloadBuffer,
				generateLoadPayloadSize,
				false
			);
		}
	}

}

void Node::KeepHighDiscoveryActive()
{
	//If discovery is turned off, we should not turn it on
	if (currentDiscoveryState == DiscoveryState::OFF) return;

	//Reset the state in discovery high, if anything in the cluster configuration changed
	if(currentDiscoveryState == DiscoveryState::HIGH){
		currentStateTimeoutDs = SEC_TO_DS(Conf::getInstance().highToLowDiscoveryTimeSec);
	} else {
		ChangeState(DiscoveryState::HIGH);
	}
}

/*
 #########################################################################################################
 ### Helper functions
 #########################################################################################################
 */
#define ________________HELPERS___________________

//Generates a new ClusterId by using connectionLoss and the unique id of the node
ClusterId Node::GenerateClusterID(void) const
{
	//Combine connection loss and nodeId to generate a unique cluster id
	ClusterId newId = configuration.nodeId + ((this->connectionLossCounter + randomBootNumber) << 16);

	logt("NODE", "New cluster id generated %x", newId);
	return newId;
}

bool Node::GetKey(FmKeyId fmKeyId, u8* keyOut) const
{
	if(fmKeyId == FmKeyId::NODE){
		CheckedMemcpy(keyOut, RamConfig->GetNodeKey(), 16);
		return true;
	} else if(fmKeyId == FmKeyId::NETWORK){
		CheckedMemcpy(keyOut, GS->node.configuration.networkKey, 16);
		return true;
	} else if(fmKeyId == FmKeyId::ORGANIZATION){
		CheckedMemcpy(keyOut, GS->node.configuration.organizationKey, 16);
		return true;
	} else if (fmKeyId == FmKeyId::RESTRAINED) {
		RamConfig->GetRestrainedKey(keyOut);
		return true;
	} else if(fmKeyId >= FmKeyId::USER_DERIVED_START && fmKeyId <= FmKeyId::USER_DERIVED_END){
		//Construct some cleartext with the user id to construct the user key
		u8 cleartext[16];
		CheckedMemset(cleartext, 0x00, 16);
		CheckedMemcpy(cleartext, &fmKeyId, 4);

		Utility::Aes128BlockEncrypt(
				(Aes128Block*)cleartext,
				(Aes128Block*)GS->node.configuration.userBaseKey,
				(Aes128Block*)keyOut);

		return true;
	} else {
		return false;
	}
}

Module* Node::GetModuleById(ModuleId id) const
{
	for(u32 i=0; i<GS->amountOfModules; i++){
		if(GS->activeModules[i]->moduleId == id){
			return GS->activeModules[i];
		}
	}
	return nullptr;
}

void Node::PrintStatus(void) const
{
	const FruityHal::BleGapAddr addr = FruityHal::GetBleGapAddress();

	trace("**************" EOL);
	trace("Node %s (nodeId: %u) vers: %u, NodeKey: %02X:%02X:....:%02X:%02X" EOL EOL, RamConfig->GetSerialNumber(), configuration.nodeId, GS->config.getFruityMeshVersion(),
			RamConfig->GetNodeKey()[0], RamConfig->GetNodeKey()[1], RamConfig->GetNodeKey()[14], RamConfig->GetNodeKey()[15]);
	SetTerminalTitle();
	trace("Mesh clusterSize:%u, clusterId:%u" EOL, clusterSize, clusterId);
	trace("Enrolled %u: networkId:%u, deviceType:%u, NetKey %02X:%02X:....:%02X:%02X, UserBaseKey %02X:%02X:....:%02X:%02X" EOL,
			(u32)configuration.enrollmentState, configuration.networkId, (u32)GET_DEVICE_TYPE(),
			configuration.networkKey[0], configuration.networkKey[1], configuration.networkKey[14], configuration.networkKey[15],
			configuration.userBaseKey[0], configuration.userBaseKey[1], configuration.userBaseKey[14], configuration.userBaseKey[15]);
	trace("Addr:%02X:%02X:%02X:%02X:%02X:%02X, ConnLossCounter:%u, AckField:%u, State: %u" EOL EOL,
			addr.addr[5], addr.addr[4], addr.addr[3], addr.addr[2], addr.addr[1], addr.addr[0],
			connectionLossCounter, currentAckId, (u32)currentDiscoveryState);

	//Print connection info
	BaseConnections conns = GS->cm.GetBaseConnections(ConnectionDirection::INVALID);
	trace("CONNECTIONS %u (freeIn:%u, freeOut:%u, pendingPackets:%u" EOL, conns.count, GS->cm.freeMeshInConnections, GS->cm.freeMeshOutConnections, GS->cm.GetPendingPackets());
	for (u32 i = 0; i < conns.count; i++) {
		BaseConnection *conn = conns.handles[i].GetConnection();
		conn->PrintStatus();
	}
	trace("**************" EOL);
}

void Node::SetTerminalTitle() const
{
#if IS_ACTIVE(SET_TERMINAL_TITLE)
	//Change putty terminal title
	if(Conf::getInstance().terminalMode == TerminalMode::PROMPT) trace("\033]0;Node %u (%s) ClusterSize:%d (%x), [%u, %u, %u, %u]\007",
			configuration.nodeId,
			RamConfig->serialNumber,
			clusterSize, clusterId,
			GS->cm->allConnections[0] != nullptr ? GS->cm.allConnections[0]->partnerId : 0,
			GS->cm->allConnections[1] != nullptr ? GS->cm.allConnections[1]->partnerId : 0,
			GS->cm->allConnections[2] != nullptr ? GS->cm.allConnections[2]->partnerId : 0,
			GS->cm->allConnections[3] != nullptr ? GS->cm.allConnections[3]->partnerId : 0);
#endif
}

CapabilityEntry Node::GetCapability(u32 index, bool firstCall)
{
	if (index == 0) 
	{
		CapabilityEntry retVal;
		CheckedMemset(&retVal, 0, sizeof(retVal));
		retVal.type = CapabilityEntryType::SOFTWARE;
		strcpy(retVal.manufacturer, "M-Way Solutions GmbH");
		strcpy(retVal.modelName   , "BlueRange Node");
		snprintf(retVal.revision, sizeof(retVal.revision), "%d.%d.%d", FM_VERSION_MAJOR, FM_VERSION_MINOR, FM_VERSION_PATCH);
		return retVal;
	}
	else
	{
		return Module::GetCapability(index, firstCall);
	}
}

CapabilityEntry Node::GetNextGlobalCapability()
{
	CapabilityEntry retVal;
	retVal.type = CapabilityEntryType::INVALID;
	if (!isSendingCapabilities)
	{
		SIMEXCEPTION(IllegalStateException);
		return retVal;
	}

	while (retVal.type == CapabilityEntryType::INVALID && capabilityRetrieverModuleIndex < GS->amountOfModules)
	{
		retVal = GS->activeModules[capabilityRetrieverModuleIndex]->GetCapability(capabilityRetrieverLocal, firstCallForCurrentCapabilityModule);
		firstCallForCurrentCapabilityModule = false;
		if (retVal.type == CapabilityEntryType::INVALID) 
		{
			capabilityRetrieverLocal = 0;
			capabilityRetrieverModuleIndex++;
			firstCallForCurrentCapabilityModule = true;
		}
		else if (retVal.type == CapabilityEntryType::NOT_READY)
		{
			//Do nothing, will retry again shortly.
		}
		else
		{
			capabilityRetrieverLocal++;
			capabilityRetrieverGlobal++;
		}
	}

	if (retVal.type == CapabilityEntryType::INVALID)
	{
		isSendingCapabilities = false;
		firstCallForCurrentCapabilityModule = false;
	}
	return retVal;
}

void Node::PrintBufferStatus(void) const
{
	//Print JOIN_ME buffer
	trace("JOIN_ME Buffer:" EOL);
	for (u32 i = 0; i < joinMePackets.size(); i++)
	{
		const joinMeBufferPacket* packet = &joinMePackets[i];
		trace("=> %d, clstId:%u, clstSize:%d, freeIn:%u, freeOut:%u, writeHndl:%u, ack:%u, rssi:%d, ageDs:%d", packet->payload.sender, packet->payload.clusterId, packet->payload.clusterSize, packet->payload.freeMeshInConnections, packet->payload.freeMeshOutConnections, packet->payload.meshWriteHandle, packet->payload.ackField, packet->rssi, GS->appTimerDs - packet->receivedTimeDs);
		if (packet->advType == FruityHal::BleGapAdvType::ADV_IND)
		trace(" ADV_IND" EOL);
		else if (packet->advType == FruityHal::BleGapAdvType::ADV_NONCONN_IND)
		trace(" NON_CONN" EOL);
		else
		trace(" OTHER" EOL);
	}

	trace("**************" EOL);
}


/*
 #########################################################################################################
 ### Terminal Methods
 #########################################################################################################
 */

#ifdef TERMINAL_ENABLED
TerminalCommandHandlerReturnType Node::TerminalCommandHandler(const char* commandArgs[], u8 commandArgsSize)
{
	//React on commands, return true if handled, false otherwise
	if(commandArgsSize >= 3 && TERMARGS(2 , "node"))
	{
		if(TERMARGS(0 ,"action"))
		{
			//Rewrite "this" to our own node id, this will actually build the packet
			//But reroute it to our own node
			const NodeId destinationNode = Utility::TerminalArgumentToNodeId(commandArgs[1]);

			if(commandArgsSize >= 5 && TERMARGS(3 ,"discovery"))
			{
				u8 discoveryState = (TERMARGS(4 , "off")) ? 0 : 1;

				SendModuleActionMessage(
					MessageType::MODULE_TRIGGER_ACTION,
					destinationNode,
					(u8)NodeModuleTriggerActionMessages::SET_DISCOVERY,
					0,
					&discoveryState,
					1,
					false
				);

				return TerminalCommandHandlerReturnType::SUCCESS;
			}
			//Send a reset command to a node in the mesh, it will then reboot
			if (commandArgsSize > 3 && TERMARGS(3, "reset"))
			{
				NodeModuleResetMessage data;
				data.resetSeconds = commandArgsSize > 4 ? Utility::StringToU8(commandArgs[4]) : 10;

				SendModuleActionMessage(
					MessageType::MODULE_TRIGGER_ACTION,
					destinationNode,
					(u8)NodeModuleTriggerActionMessages::RESET_NODE,
					0,
					(u8*)&data,
					SIZEOF_NODE_MODULE_RESET_MESSAGE,
					false
				);

				return TerminalCommandHandlerReturnType::SUCCESS;
			}

			if (commandArgsSize > 3 && TERMARGS(3, "ping"))
			{
				const u8 requestHandle = commandArgsSize > 4 ? Utility::StringToU8(commandArgs[4]) : 0;
				SendModuleActionMessage(
					MessageType::MODULE_TRIGGER_ACTION,
					destinationNode,
					(u8)NodeModuleTriggerActionMessages::PING,
					requestHandle,
					nullptr,
					0,
					false
				);

				return TerminalCommandHandlerReturnType::SUCCESS;
			}

			if (commandArgsSize > 7 && TERMARGS(3, "generate_load"))
			{
				//  0     1    2        3          4     5      6            7                 8
				//action this node generate_load target size repeats timeBetweenMessages {requestHandle}
				GenerateLoadTriggerMessage gltm;
				CheckedMemset(&gltm, 0, sizeof(GenerateLoadTriggerMessage));
				gltm.target                = Utility::StringToU16(commandArgs[4]);
				gltm.size                  = Utility::StringToU8 (commandArgs[5]);
				gltm.amount                = Utility::StringToU8 (commandArgs[6]);
				gltm.timeBetweenMessagesDs = Utility::StringToU8 (commandArgs[7]);

				const u8 requestHandle = commandArgsSize > 8 ? Utility::StringToU8(commandArgs[8]) : 0;
				SendModuleActionMessage(
					MessageType::MODULE_TRIGGER_ACTION,
					destinationNode,
					(u8)NodeModuleTriggerActionMessages::START_GENERATE_LOAD,
					requestHandle,
					(u8*)&gltm,
					sizeof(gltm),
					false
				);

				return TerminalCommandHandlerReturnType::SUCCESS;
			}

			if (
				   commandArgsSize >= 5 
				&& commandArgsSize <= 5 + Conf::MAX_AMOUNT_PREFERRED_PARTNER_IDS 
				&& TERMARGS(3, "set_preferred_connections")
				)
			{
				PreferredConnectionMessage message;
				if (TERMARGS(4, "ignored"))
				{
					message.preferredConnectionMode = PreferredConnectionMode::IGNORED;
				}
				else if (TERMARGS(4, "penalty"))
				{
					message.preferredConnectionMode = PreferredConnectionMode::PENALTY;
				}
				else
				{
					SIMEXCEPTION(IllegalArgumentException); //LCOV_EXCL_LINE assertion
					return TerminalCommandHandlerReturnType::WRONG_ARGUMENT;
				}
				message.preferredConnectionMode = (TERMARGS(4, "ignored")) ? PreferredConnectionMode::IGNORED : PreferredConnectionMode::PENALTY;
				message.amountOfPreferredPartnerIds = commandArgsSize - 5;

				if (message.amountOfPreferredPartnerIds > Conf::MAX_AMOUNT_PREFERRED_PARTNER_IDS) 
				{
					SIMEXCEPTION(IllegalArgumentException);
					return TerminalCommandHandlerReturnType::WRONG_ARGUMENT;
				}

				bool didError = false;
				for (size_t i = 0; i < message.amountOfPreferredPartnerIds; i++)
				{
					message.preferredPartnerIds[i] = Utility::StringToU16(commandArgs[5 + i], &didError);
				}

				if (didError) return TerminalCommandHandlerReturnType::WRONG_ARGUMENT;

				SendModuleActionMessage(
					MessageType::MODULE_TRIGGER_ACTION,
					destinationNode,
					(u8)NodeModuleTriggerActionMessages::SET_PREFERRED_CONNECTIONS,
					0,
					(u8*)&message,
					sizeof(PreferredConnectionMessage),
					false
				);

				return TerminalCommandHandlerReturnType::SUCCESS;
			}
		}
	}

#if IS_INACTIVE(CLC_GW_SAVE_SPACE)	//If you require a reset, use action reset instead
	/************* SYSTEM ***************/
	if (TERMARGS(0 ,"reset"))
	{
		Reboot(1, RebootReason::LOCAL_RESET);
		return TerminalCommandHandlerReturnType::SUCCESS;
	}
#endif
	/************* NODE ***************/
	//Get a full status of the node
#if IS_INACTIVE(GW_SAVE_SPACE)
	else if (TERMARGS(0, "status"))
	{
		PrintStatus();

		return TerminalCommandHandlerReturnType::SUCCESS;
	}
	//Allows us to send arbitrary mesh packets
	else if (TERMARGS(0, "rawsend") && commandArgsSize > 1) {
		DYNAMIC_ARRAY(buffer, 200);
		u32 len = Logger::parseEncodedStringToBuffer(commandArgs[1], buffer, 200);

		//TODO: We could optionally allow to specify delivery priority and reliability

		GS->cm.SendMeshMessage(buffer, len, DeliveryPriority::LOW);

		return TerminalCommandHandlerReturnType::SUCCESS;
	}
#ifdef SIM_ENABLED
	//Allows us to send arbitrary mesh packets and queue them directly without other checks
	//MUST NOT BE USED EXCEPT FOR TESTING
	else if (TERMARGS(0, "rawsend_high") && commandArgsSize > 1) {
		DYNAMIC_ARRAY(buffer, 200);
		u32 len = Logger::parseEncodedStringToBuffer(commandArgs[1], buffer, 200);

		//Because the implementation doesn't easily allow us to send WRITE_REQ to all connections, we have to work around that
		BaseConnections conns = GS->cm.GetBaseConnections(ConnectionDirection::INVALID);
		for (u32 i = 0; i < conns.count; i++)
		{
			BaseConnection* conn = conns.handles[i].GetConnection();
			if (!conn) continue;
			
			if (conn->connectionType == ConnectionType::FRUITYMESH) {
				MeshConnection* mconn = (MeshConnection*)conn;
				mconn->SendHandshakeMessage(buffer, len, true);
			}
			else if (conn->connectionType == ConnectionType::MESH_ACCESS) {
				MeshAccessConnection* mconn = (MeshAccessConnection*)conn;
				mconn->SendData(buffer, len, DeliveryPriority::MESH_INTERNAL_HIGH, true);
			}
		}

		return TerminalCommandHandlerReturnType::SUCCESS;
	}
#endif
#endif
	else if (commandArgsSize >= 5 && commandArgsSize <= 6 && TERMARGS(0, "raw_data_light"))
	{
		//Command description
		//Index               0           1                2               3           4            5       
		//Name        raw_data_light [receiverId] [destinationModule] [protocolId] [payload] {requestHandle}
		//Type             string        u16              u8               u8      hexstring       u8       

		alignas(RawDataLight) u8 buffer[120 + sizeof(RawDataLight)];
		CheckedMemset(&buffer, 0, sizeof(buffer));
		RawDataLight& packet = (RawDataLight&)buffer;

		if (commandArgsSize >= 6)
		{
			packet.requestHandle = Utility::StringToU8(commandArgs[5]);
		}

		packet.connHeader.messageType = MessageType::MODULE_RAW_DATA_LIGHT;
		packet.connHeader.sender = configuration.nodeId;
		packet.connHeader.receiver = Utility::TerminalArgumentToNodeId(commandArgs[1]);

		packet.moduleId = (ModuleId)Utility::StringToU8(commandArgs[2]);
		packet.protocolId = static_cast<RawDataProtocol>(Utility::StringToU8(commandArgs[3]));

		u32 payloadLength = Logger::parseEncodedStringToBuffer(commandArgs[4], packet.payload, sizeof(buffer) - sizeof(RawDataLight) + 1);

		//Let's do some sanity checks!
		if (payloadLength == 0)	//Nothing to send
			return TerminalCommandHandlerReturnType::WRONG_ARGUMENT;

		GS->cm.SendMeshMessage(
			buffer,
			sizeof(RawDataLight) - 1 + payloadLength,
			DeliveryPriority::LOW
			);

		return TerminalCommandHandlerReturnType::SUCCESS;
	}
	//Send some large data that is split over a few messages
	else if(commandArgsSize >= 5 && commandArgsSize <= 6 && TERMARGS(0, "raw_data_start"))
	{
		//Command description
		//Index            0              1                2               3           4             5
		//Name        raw_data_start [receiverId] [destinationModule] [numChunks] [protocolId] {requestHandle}
		//Type          string           u16              u8              u24          u8            u8

		RawDataStart paket;
		CheckedMemset(&paket, 0, sizeof(paket));
		if (!CreateRawHeader(&paket.header, RawDataActionType::START, commandArgs, commandArgsSize >= 6 ? commandArgs[5] : nullptr))
			return TerminalCommandHandlerReturnType::WRONG_ARGUMENT;

		paket.numChunks   = Utility::StringToU32(commandArgs[3]);
		paket.protocolId = (u32)static_cast<RawDataProtocol>(Utility::StringToU8(commandArgs[4]));

		//paket.reserved;    Leave zero

		GS->cm.SendMeshMessage(
			(u8*)&paket,
			sizeof(RawDataStart),
			DeliveryPriority::LOW
			);

		return TerminalCommandHandlerReturnType::SUCCESS;
	}
	else if (commandArgsSize >= 5 && commandArgsSize <= 6 && TERMARGS(0, "raw_data_error"))
	{
		//Command description
		//Index               0            1               2                3           4              5
		//Name        raw_data_error [receiverId] [destinationModule] [errorCode] [destination] {requestHandle}
		//Type             string         u16             u8               u8          u8             u8
		
		bool didError = false;

		NodeId                  receiver                =                          Utility::TerminalArgumentToNodeId(commandArgs[1]);
		ModuleId                moduleId                = (ModuleId)               Utility::StringToU8 (commandArgs[2], &didError);
		RawDataErrorType        rawDataErrorType        = (RawDataErrorType)       Utility::StringToU8 (commandArgs[3], &didError);
		RawDataErrorDestination rawDataErrorDestination = (RawDataErrorDestination)Utility::StringToU8 (commandArgs[4], &didError);

		u8 requestHandle = 0;
		if (commandArgsSize >= 6) 
		{
			requestHandle = Utility::StringToU8(commandArgs[5], &didError);
		}

		if (didError || receiver == NODE_ID_INVALID) return TerminalCommandHandlerReturnType::WRONG_ARGUMENT;

		SendRawError(receiver, moduleId, rawDataErrorType, rawDataErrorDestination,	requestHandle);

		return TerminalCommandHandlerReturnType::SUCCESS;

	}
	else if (commandArgsSize >= 3 && commandArgsSize <= 4 && TERMARGS(0, "raw_data_start_received"))
	{
		//Command description
		//Index                  0                 1                2                 3
		//Name        raw_data_start_received [receiverId] [destinationModule] {requestHandle}
		//Type                string              u16              u8                 u8

		RawDataStartReceived paket;
		CheckedMemset(&paket, 0, sizeof(paket));
		if (!CreateRawHeader(&paket.header, RawDataActionType::START_RECEIVED, commandArgs, commandArgsSize >= 4 ? commandArgs[3] : nullptr))
			return TerminalCommandHandlerReturnType::WRONG_ARGUMENT;

		GS->cm.SendMeshMessage(
			(u8*)&paket,
			sizeof(RawDataStartReceived),
			DeliveryPriority::LOW
			);

		return TerminalCommandHandlerReturnType::SUCCESS;
	}
	else if (commandArgsSize >= 5 && commandArgsSize <= 6 && TERMARGS(0, "raw_data_chunk"))
	{
		//Command description
		//Index               0           1                2              3         4            5       
		//Name        raw_data_chunk [receiverId] [destinationModule] [chunkId] [payload] {requestHandle}
		//Type             string        u16              u8             u24    hexstring       u8       

		alignas(RawDataChunk) u8 buffer[120 + sizeof(RawDataChunk)];
		CheckedMemset(&buffer, 0, sizeof(buffer));
		RawDataChunk& packet = (RawDataChunk&)buffer;
		if (!CreateRawHeader(&packet.header, RawDataActionType::CHUNK, commandArgs, commandArgsSize >= 6 ? commandArgs[5] : nullptr))
			return TerminalCommandHandlerReturnType::WRONG_ARGUMENT;

		packet.chunkId = Utility::StringToU32(commandArgs[3]);
		//paket.reserved;    Leave zero

		u32 payloadLength = Logger::parseEncodedStringToBuffer(commandArgs[4], packet.payload, sizeof(buffer) - sizeof(RawDataChunk) + 1);

		//Let's do some sanity checks!
		if (payloadLength == 0)	//Nothing to send
			return TerminalCommandHandlerReturnType::WRONG_ARGUMENT;
		if (((strlen(commandArgs[4]) + 1) / 3) > MAX_RAW_CHUNK_SIZE)	//Msg too long
			return TerminalCommandHandlerReturnType::WRONG_ARGUMENT;

		GS->cm.SendMeshMessage(
			buffer,
			sizeof(RawDataChunk) - 1 + payloadLength,
			DeliveryPriority::LOW
			);

		return TerminalCommandHandlerReturnType::SUCCESS;
	}
	else if (commandArgsSize >= 4 && commandArgsSize <= 5 && TERMARGS(0, "raw_data_report"))
	{
		RawDataReport paket;
		CheckedMemset(&paket, 0, sizeof(paket));
		if (!CreateRawHeader(&paket.header, RawDataActionType::REPORT, commandArgs, commandArgsSize >= 5 ? commandArgs[4] : nullptr))
			return TerminalCommandHandlerReturnType::WRONG_ARGUMENT;

		if (strcmp(commandArgs[3], "-") != 0) 
		{
			const char* readPtr = commandArgs[3];
			paket.missings[0] = strtoul(readPtr, nullptr, 0);
			int missingIndex = 1;
			while (*readPtr != '\0')
			{
				if (*readPtr == ',')
				{
					if (missingIndex == sizeof(paket.missings) / sizeof(paket.missings[0])) //Too many missings
					{
						return TerminalCommandHandlerReturnType::WRONG_ARGUMENT; //LCOV_EXCL_LINE assertion
					}
					paket.missings[missingIndex] = strtoul(readPtr + 1, nullptr, 0);
					missingIndex++;
				}
				readPtr++;
			}
				}

		GS->cm.SendMeshMessage(
			(u8*)&paket,
			sizeof(RawDataReport),
			DeliveryPriority::LOW
			);

		return TerminalCommandHandlerReturnType::SUCCESS;
	}
	else if (commandArgsSize >= 2 && TERMARGS(0, "request_capability"))
	{
		CapabilityRequestedMessage message;
		CheckedMemset(&message, 0, sizeof(message));
		message.header.header.messageType = MessageType::CAPABILITY;
		message.header.header.sender      = configuration.nodeId;
		message.header.header.receiver    = Utility::StringToU16(commandArgs[1]);
		message.header.actionType         = CapabilityActionType::REQUESTED;

		//We don't allow broadcasts of the capability request
		//as it would put the mesh under heavy load.
		if (message.header.header.receiver == NODE_ID_BROADCAST)
		{
			return TerminalCommandHandlerReturnType::WRONG_ARGUMENT;
		}

		GS->cm.SendMeshMessage(
			(u8*)&message,
			sizeof(CapabilityRequestedMessage),
			DeliveryPriority::LOW
		);
		return TerminalCommandHandlerReturnType::SUCCESS;
	}
	//Set a timestamp for this node
	else if (TERMARGS(0, "settime") && commandArgsSize >= 3)
	{
		//Set the time for our node
		GS->timeManager.SetTime(strtoul(commandArgs[1], nullptr, 10), 0, (i16)strtoul(commandArgs[2], nullptr, 10));

		return TerminalCommandHandlerReturnType::SUCCESS;
	}
#if IS_INACTIVE(CLC_GW_SAVE_SPACE)
	//Display the time of this node
	else if(TERMARGS(0, "gettime"))
	{
		char timestring[80];
		GS->timeManager.convertTimestampToString(timestring);

		if (GS->timeManager.IsTimeSynced())
		{
			trace("Time is currently %s" EOL, timestring);		
		}
		else
		{
			trace("Time is currently not set: %s" EOL, timestring);	
		}
		return TerminalCommandHandlerReturnType::SUCCESS;
	}
	else if (TERMARGS(0, "startterm"))
	{
		Conf::getInstance().terminalMode = TerminalMode::PROMPT;
		return TerminalCommandHandlerReturnType::SUCCESS;
	}
#endif
	else if (TERMARGS(0, "stopterm"))
	{
		Conf::getInstance().terminalMode = TerminalMode::JSON;
		return TerminalCommandHandlerReturnType::SUCCESS;
	}

	else if (TERMARGS(0, "set_serial") && commandArgsSize == 2)
	{
		if (strlen(commandArgs[1]) != 5) return TerminalCommandHandlerReturnType::WRONG_ARGUMENT;

		u32 serial = Utility::GetIndexForSerial(commandArgs[1]);
		if (serial == INVALID_SERIAL_NUMBER_INDEX) return TerminalCommandHandlerReturnType::WRONG_ARGUMENT;

		GS->config.SetSerialNumberIndex(serial);

		logt("NODE", "Serial Number Index set to %u", serial);

		return TerminalCommandHandlerReturnType::SUCCESS;
	}

	else if (TERMARGS(0, "set_node_key") && commandArgsSize == 2)
	{
		u8 key[16];
		const u32 length = Logger::parseEncodedStringToBuffer(commandArgs[1], key, sizeof(key));
		
		if (length != 16) return TerminalCommandHandlerReturnType::WRONG_ARGUMENT;

		GS->config.SetNodeKey(key);

		logt("NODE", "Node Key set to %s", commandArgs[1]);

		return TerminalCommandHandlerReturnType::SUCCESS;
	}


	/************* Debug commands ***************/
	else if (TERMARGS(0,"component_sense") && commandArgsSize >= 7)
	{
		u8 buffer[200];
		connPacketComponentMessage* message = (connPacketComponentMessage*)buffer;
		message->componentHeader.header.messageType = MessageType::COMPONENT_SENSE;
		message->componentHeader.header.sender = configuration.nodeId;
		message->componentHeader.header.receiver = Utility::TerminalArgumentToNodeId(commandArgs[1]);
		message->componentHeader.moduleId = (ModuleId)strtoul(commandArgs[2], nullptr, 0);
		message->componentHeader.actionType = (u8)strtoul(commandArgs[3], nullptr, 0);
		message->componentHeader.component = (u16)strtoul(commandArgs[4], nullptr, 0);
		message->componentHeader.registerAddress = (u16)strtoul(commandArgs[5], nullptr, 0);
		u8 length = Logger::parseEncodedStringToBuffer(commandArgs[6], message->payload, sizeof(buffer) - SIZEOF_COMPONENT_MESSAGE_HEADER);
		message->componentHeader.requestHandle = (u8)((commandArgsSize > 7) ? strtoul(commandArgs[7], nullptr, 0) : 0);

		SendComponentMessage(*message, length);
		return TerminalCommandHandlerReturnType::SUCCESS;
	}

	else if (TERMARGS(0,"component_act") && commandArgsSize >= 7)
	{
		u8 buffer[200];
		connPacketComponentMessage* message = (connPacketComponentMessage*)buffer;
		message->componentHeader.header.messageType = MessageType::COMPONENT_ACT;
		message->componentHeader.header.sender = configuration.nodeId;
		message->componentHeader.header.receiver = Utility::TerminalArgumentToNodeId(commandArgs[1]);
		message->componentHeader.moduleId = (ModuleId)strtoul(commandArgs[2], nullptr, 0);
		message->componentHeader.actionType = (u8)strtoul(commandArgs[3], nullptr, 0);
		message->componentHeader.component = (u16)strtoul(commandArgs[4], nullptr, 0);
		message->componentHeader.registerAddress = (u16)strtoul(commandArgs[5], nullptr, 0);
		message->componentHeader.requestHandle = (u8)((commandArgsSize > 7) ? strtoul(commandArgs[7], nullptr, 0) : 0);
		u8 length = Logger::parseEncodedStringToBuffer(commandArgs[6], message->payload, sizeof(buffer) - SIZEOF_COMPONENT_MESSAGE_HEADER);

		SendComponentMessage(*message, length);
		return TerminalCommandHandlerReturnType::SUCCESS;
	}
#if IS_INACTIVE(SAVE_SPACE)
	//Print the JOIN_ME buffer
	else if (TERMARGS(0, "bufferstat"))
	{
		PrintBufferStatus();
		return TerminalCommandHandlerReturnType::SUCCESS;
	}
	//Send some large data that is split over a few messages
	else if(TERMARGS(0, "datal"))
	{
		bool reliable = (commandArgsSize > 1 && TERMARGS(1 ,"r"));

		const u8 dataLength = 145;
		u8 _packet[dataLength];
		connPacketHeader* packet = (connPacketHeader*)_packet;
		packet->messageType = MessageType::DATA_1;
		packet->receiver = 0;
		packet->sender = configuration.nodeId;

		for(u32 i=0; i< dataLength-5; i++){
			_packet[i+5] = i+1;
		}
		
		ErrorType err = GS->cm.SendMeshMessageInternal(_packet, dataLength, DeliveryPriority::LOW, reliable, true, true);
		if (err == ErrorType::SUCCESS) return TerminalCommandHandlerReturnType::SUCCESS;
		else return TerminalCommandHandlerReturnType::INTERNAL_ERROR;
	}
#if IS_INACTIVE(GW_SAVE_SPACE)
	//Stop the state machine
	else if (TERMARGS(0, "stop"))
	{
		DisableStateMachine(true);
		logt("NODE", "Stopping state machine.");
		return TerminalCommandHandlerReturnType::SUCCESS;
	}
	//Start the state machine
	else if (TERMARGS(0, "start"))
	{
		DisableStateMachine(false);
		logt("NODE", "Starting state machine.");

		return TerminalCommandHandlerReturnType::SUCCESS;
	}
#endif
	//Try to connect to one of the nodes in the test devices array
	else if (TERMARGS(0, "connect"))
	{
		if(commandArgsSize <= 2) return TerminalCommandHandlerReturnType::NOT_ENOUGH_ARGUMENTS;

		//Allows us to connect to any node when giving the GAP Address
		bool didError = false;
		NodeId partnerId = Utility::StringToU16(commandArgs[1], &didError);
		u8 buffer[6];
		Logger::parseEncodedStringToBuffer(commandArgs[2], buffer, 6, &didError);
		FruityHal::BleGapAddr addr;
		addr.addr_type = FruityHal::BleGapAddrType::RANDOM_STATIC;
		addr.addr[0] = buffer[5];
		addr.addr[1] = buffer[4];
		addr.addr[2] = buffer[3];
		addr.addr[3] = buffer[2];
		addr.addr[4] = buffer[1];
		addr.addr[5] = buffer[0];

		if (didError)
		{
			return TerminalCommandHandlerReturnType::WRONG_ARGUMENT;
		}

		//Using the same GATT handle as our own will probably work if our partner has the same implementation
		const ErrorType err = GS->cm.ConnectAsMaster(partnerId, &addr, meshService.sendMessageCharacteristicHandle.valueHandle, MSEC_TO_UNITS(10, CONFIG_UNIT_1_25_MS));

		if (err != ErrorType::SUCCESS)
		{
			logt("NODE", "Failed to connect as master because %u", (u32)err);
			if (err == ErrorType::INVALID_ADDR)
			{
				return TerminalCommandHandlerReturnType::WRONG_ARGUMENT;
			}
			return TerminalCommandHandlerReturnType::INTERNAL_ERROR;
		}

		return TerminalCommandHandlerReturnType::SUCCESS;
	}
#endif

#if IS_INACTIVE(SAVE_SPACE)
	//Disconnect a connection by its handle or all
	else if (TERMARGS(0, "disconnect"))
	{
		if(commandArgsSize <= 1) return TerminalCommandHandlerReturnType::NOT_ENOUGH_ARGUMENTS;
		if(TERMARGS(1 , "all")){
			GS->cm.ForceDisconnectAllConnections(AppDisconnectReason::USER_REQUEST);
		} else {
			BaseConnectionHandle conn = GS->cm.GetConnectionFromHandle(Utility::StringToU16(commandArgs[1]));
			if(conn){
				conn.DisconnectAndRemove(AppDisconnectReason::USER_REQUEST);
			}
		}

		return TerminalCommandHandlerReturnType::SUCCESS;
	}
	//tell the gap layer to loose a connection
	else if (TERMARGS(0, "gap_disconnect"))
	{
		if(commandArgsSize <= 1) return TerminalCommandHandlerReturnType::NOT_ENOUGH_ARGUMENTS;
		u16 connectionHandle = Utility::StringToU16(commandArgs[1]);
		const ErrorType err = FruityHal::Disconnect(connectionHandle, FruityHal::BleHciError::REMOTE_USER_TERMINATED_CONNECTION);

		if (err != ErrorType::SUCCESS)
		{
			if (err == ErrorType::BLE_INVALID_CONN_HANDLE) return TerminalCommandHandlerReturnType::WRONG_ARGUMENT;
			return TerminalCommandHandlerReturnType::INTERNAL_ERROR;
		}

		return TerminalCommandHandlerReturnType::SUCCESS;
	}
	else if(TERMARGS(0, "update_iv")) 	//jstodo can this be removed? Currently untested
	{
		if(commandArgsSize <= 2) return TerminalCommandHandlerReturnType::NOT_ENOUGH_ARGUMENTS;

		NodeId nodeId = Utility::StringToU16(commandArgs[1]);
		u16 newConnectionInterval = Utility::StringToU16(commandArgs[2]);

		connPacketUpdateConnectionInterval packet;
		packet.header.messageType = MessageType::UPDATE_CONNECTION_INTERVAL;
		packet.header.sender = GS->node.configuration.nodeId;
		packet.header.receiver = nodeId;

		packet.newInterval = newConnectionInterval;
		ErrorType err = GS->cm.SendMeshMessageInternal((u8*)&packet, SIZEOF_CONN_PACKET_UPDATE_CONNECTION_INTERVAL, DeliveryPriority::MESH_INTERNAL_HIGH, true, true, true);
		if (err == ErrorType::SUCCESS) return TerminalCommandHandlerReturnType::SUCCESS;
		else return TerminalCommandHandlerReturnType::INTERNAL_ERROR;
	}
#endif
	/************* UART COMMANDS ***************/
	//Get the status information of this node
	else if(TERMARGS(0, "get_plugged_in"))
	{
		logjson("NODE", "{\"type\":\"plugged_in\",\"nodeId\":%u,\"serialNumber\":\"%s\",\"fmVersion\":%u}" SEP, configuration.nodeId, RamConfig->GetSerialNumber(), FM_VERSION);
		return TerminalCommandHandlerReturnType::SUCCESS;
	}
#if IS_INACTIVE(SAVE_SPACE)
	//Query all modules from any node
	else if((TERMARGS(0, "get_modules")))
	{
		if(commandArgsSize <= 1) return TerminalCommandHandlerReturnType::NOT_ENOUGH_ARGUMENTS;

		NodeId receiver = Utility::TerminalArgumentToNodeId(commandArgs[1]);

		connPacketModule packet;
		packet.header.messageType = MessageType::MODULE_CONFIG;
		packet.header.sender = configuration.nodeId;
		packet.header.receiver = receiver;

		packet.moduleId = ModuleId::NODE;
		packet.requestHandle = 0;
		packet.actionType = (u8)Module::ModuleConfigMessages::GET_MODULE_LIST;

		GS->cm.SendMeshMessage((u8*) &packet, SIZEOF_CONN_PACKET_MODULE, DeliveryPriority::LOW);

		return TerminalCommandHandlerReturnType::SUCCESS;
	}
#endif
#if IS_INACTIVE(GW_SAVE_SPACE)
	else if(TERMARGS(0, "sep")){
		trace(EOL);
		for(u32 i=0; i<80*5; i++){
			if(i%80 == 0) trace(EOL);
			trace("#");
		}
		trace(EOL);
		trace(EOL);
		return TerminalCommandHandlerReturnType::SUCCESS;
	}
#endif
	else if (TERMARGS(0, "enable_corruption_check"))
	{
		logjson("NODE", "{\"type\":\"enable_corruption_check_response\",\"err\":0,\"check\":\"crc32\"}" SEP);
		GS->terminal.EnableCrcChecks();
		return TerminalCommandHandlerReturnType::SUCCESS;
	}

	//Must be called to allow the module to get and set the config
	return Module::TerminalCommandHandler(commandArgs, commandArgsSize);
}
#endif

inline void Node::SendModuleList(NodeId toNode, u8 requestHandle) const
{
	u8 buffer[SIZEOF_CONN_PACKET_MODULE + (MAX_MODULE_COUNT + 1) * 4];
	CheckedMemset(buffer, 0, sizeof(buffer));

	connPacketModule* outPacket = (connPacketModule*) buffer;
	outPacket->header.messageType = MessageType::MODULE_CONFIG;
	outPacket->header.sender = configuration.nodeId;
	outPacket->header.receiver = toNode;

	outPacket->moduleId = ModuleId::NODE;
	outPacket->requestHandle = requestHandle;
	outPacket->actionType = (u8)Module::ModuleConfigMessages::MODULE_LIST;

	for(u32 i = 0; i<GS->amountOfModules; i++){
		//TODO: can we do this better? the data region is unaligned in memory
			CheckedMemcpy(outPacket->data + i*4, &GS->activeModules[i]->configurationPointer->moduleId, 1);
			CheckedMemcpy(outPacket->data + i*4 + 2, &GS->activeModules[i]->configurationPointer->moduleVersion, 1);
			CheckedMemcpy(outPacket->data + i*4 + 3, &GS->activeModules[i]->configurationPointer->moduleActive, 1);
	}

	GS->cm.SendMeshMessage(
			(u8*)outPacket,
			SIZEOF_CONN_PACKET_MODULE + (MAX_MODULE_COUNT+1)*4,
			DeliveryPriority::LOW
			);
}


bool Node::IsPreferredConnection(NodeId id) const
{
	//If we don't have preferred connections set, any connection is treated as a preferred connection (every connection is equal).
	if (GS->config.configuration.amountOfPreferredPartnerIds == 0) return true;


	for (size_t i = 0; i < GS->config.configuration.amountOfPreferredPartnerIds; i++)
	{
		if (GS->config.configuration.preferredPartnerIds[i] == id)
		{
			return true;
		}
	}
	return false;
}

void Node::SendRawError(NodeId receiver, ModuleId moduleId, RawDataErrorType type, RawDataErrorDestination destination, u8 requestHandle) const
{
	RawDataError paket;
	CheckedMemset(&paket, 0, sizeof(paket));

	paket.header.connHeader.messageType = MessageType::MODULE_RAW_DATA;
	paket.header.connHeader.sender = configuration.nodeId;
	paket.header.connHeader.receiver = receiver;

	paket.header.moduleId = moduleId;
	paket.header.actionType = RawDataActionType::ERROR_T;
	paket.header.requestHandle = requestHandle;

	paket.type = type;
	paket.destination = destination;

	GS->cm.SendMeshMessage(
		(u8*)&paket,
		sizeof(RawDataError),
		DeliveryPriority::LOW
		);
}

void Node::SendComponentMessage(connPacketComponentMessage& message, u16 payloadSize)
{
	GS->cm.SendMeshMessage((u8*)&message, SIZEOF_CONN_PACKET_COMPONENT_MESSAGE + payloadSize, DeliveryPriority::LOW);
}



bool Node::CreateRawHeader(RawDataHeader* outVal, RawDataActionType type, const char* commandArgs[], const char* requestHandle) const
{
	if (requestHandle != nullptr)
	{
		outVal->requestHandle = Utility::StringToU8(requestHandle);
	}

	outVal->connHeader.messageType = MessageType::MODULE_RAW_DATA;
	outVal->connHeader.sender = configuration.nodeId;
	outVal->connHeader.receiver = Utility::TerminalArgumentToNodeId(commandArgs[1]);

	outVal->moduleId = (ModuleId)Utility::StringToU8(commandArgs[2]);
	outVal->actionType = type;


	return true;
}

void Node::Reboot(u32 delayDs, RebootReason reason)
{
	u32 newRebootTimeDs = GS->appTimerDs + delayDs;
	// Only store the new reboot reason if it happens before the previously set reboot reason or if no reboot reason was set yet.
	// The reason for this is that if two different reboots are logically "queued", the later one has no effect, because the
	// earlier one has already taken effect, eliminating the later reboot. Thus at every time only a single reboot actually must
	// be rememberd which is the one that happens the earliest.
	if (rebootTimeDs == 0 || newRebootTimeDs < rebootTimeDs)
	{
		rebootTimeDs = newRebootTimeDs;
		GS->ramRetainStructPtr->rebootReason = reason;
	}
}

bool Node::IsRebootScheduled()
{
	return rebootTimeDs != 0;
}

/* EOF */
