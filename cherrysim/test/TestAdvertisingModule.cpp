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
#include "gtest/gtest.h"
#include <CherrySimTester.h>
#include <CherrySimUtils.h>
#include <Node.h>

TEST(TestAdvertisingModule, TestIfMessageIsBroadcasted) {
	CherrySimTesterConfig testerConfig = CherrySimTester::CreateDefaultTesterConfiguration();
	SimConfiguration simConfig = CherrySimTester::CreateDefaultSimConfiguration();
	simConfig.nodeConfigName.insert({ "prod_sink_nrf52", 1});
	simConfig.nodeConfigName.insert({ "prod_mesh_nrf52", 1 });
	simConfig.terminalId = 1;
	//testerConfig.verbose = true;

	CherrySimTester tester = CherrySimTester(testerConfig, simConfig);
	tester.Start();

	tester.SimulateUntilClusteringDone(100 * 1000);

	//Tell node 1 to broadcast an iBeacon Message
	tester.SendTerminalCommand(1, "set_config this adv 01:01:01:00:64:00:01:04:01:F0:02:01:06:1A:FF:4C:00:02:15:F0:01:8B:9B:75:09:4C:31:A9:05:1A:27:D3:9C:00:3C:EA:60:00:32:81:00 0");

	tester.SimulateUntilMessageReceived(1000, 1, "set_config_result");

	//The data of the above iBeacon message
	u8 data[] = { 0x02, 0x01, 0x06, 0x1A, 0xFF, 0x4C, 0x00, 0x02, 0x15, 0xF0, 0x01, 0x8B, 0x9B, 0x75, 0x09, 0x4C, 0x31, 0xA9, 0x05, 0x1A, 0x27, 0xD3, 0x9C, 0x00, 0x3C, 0xEA, 0x60, 0x00, 0x32, 0x81 };

	//Wait until node 2 receives an advertising message that contains this iBeacon message
	tester.SimulateUntilBleEventReceived(100 * 1000, 2, BLE_GAP_EVT_ADV_REPORT, data, sizeof(data));
}