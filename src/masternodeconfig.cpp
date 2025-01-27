
// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Darkcoin developers
// Copyright (c) 2017-2019 The LindaProject Inc developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "masternodeconfig.h"
#include "net.h"
#include "util.h"
#include "ui_interface.h"

CMasternodeConfig masternodeConfig;

void CMasternodeConfig::add(std::string alias, std::string ip, std::string privKey, std::string txHash, std::string outputIndex)
{
    CMasternodeEntry cme(alias, ip, privKey, txHash, outputIndex);
    entries.push_back(cme);
}

bool CMasternodeConfig::read(std::string& strErr)
{
    int linenumber = 1;
    entries = std::vector<CMasternodeEntry>(); //! Clear entries so we don't double up
    boost::filesystem::ifstream streamConfig(GetMasternodeConfigFile());
    if (!streamConfig.good()) {
        return true; //! No masternode.conf file is OK
    }

    for (std::string line; std::getline(streamConfig, line); linenumber++) {
        if (line.empty()) {
            continue;
        }
        std::istringstream iss(line);
        std::string alias, ip, privKey, txHash, outputIndex;
        if (!(iss >> alias >> ip >> privKey >> txHash >> outputIndex)) {
            iss.str(line);
            iss.clear();
            if (!(iss >> alias >> ip >> privKey >> txHash >> outputIndex)) {
                strErr = _("Could not parse masternode.conf") + "\n" +
                        strprintf(_("Line: %d"), linenumber) + "\n\"" + line + "\"";
                streamConfig.close();
                return false;
            }
        }

        /*        if(CService(ip).GetPort() != 19999 && CService(ip).GetPort() != 9999)  {
            strErr = "Invalid port (must be 9999 for mainnet or 19999 for testnet) detected in masternode.conf: " + line;
            streamConfig.close();
            return false;
        }*/

        add(alias, ip, privKey, txHash, outputIndex);
    }

    streamConfig.close();
    return true;
}

bool CMasternodeConfig::create(std::string alias, std::string ip, std::string privKey, std::string txHash, std::string outputIndex)
{
    //! check if already have masternode account
    bool exists = false;
    for (std::vector<CMasternodeEntry>::const_iterator it = entries.begin(); it != entries.end(); it++) {
        if ((*it).getAlias() == alias) {
            exists = true;
            break;
        }
    }

    if (!exists) {
        //! update the masternode config file
        boost::filesystem::ofstream streamConfig(GetMasternodeConfigFile());

        for (std::vector<CMasternodeEntry>::const_iterator it = entries.begin(); it != entries.end(); it++) {
            streamConfig << (*it).getAlias() << " " << (*it).getIp() << " " << (*it).getPrivKey() << " " << (*it).getTxHash() << " " << (*it).getOutputIndex() << std::endl;
        }

        //! add the new masternode to config file
        streamConfig << alias << " " << ip << " " << privKey << " " << txHash << " " << outputIndex << std::endl;

        //! add new masternode to entries
        add(alias, ip, privKey, txHash, outputIndex);

        streamConfig.close();
        return true;
    }

    return false;
}

bool CMasternodeConfig::remove(std::string alias)
{
    //! update masternode config file to remove the account
    boost::filesystem::ofstream streamConfig(GetMasternodeConfigFile());
    int rIndex = -1;
    int i = 0;

    for (std::vector<CMasternodeEntry>::const_iterator it = entries.begin(); it != entries.end(); it++) {
        i++;
        if ((*it).getAlias() != alias) {
            streamConfig << (*it).getAlias() << " " << (*it).getIp() << " " << (*it).getPrivKey() << " " << (*it).getTxHash() << " " << (*it).getOutputIndex() << std::endl;
        } else {
            rIndex = i;
        }
    }

    streamConfig.close();

    //! remove from our entries
    if (rIndex != -1) {
        entries.erase(entries.begin() + rIndex);
        return true;
    }

    return false;
}