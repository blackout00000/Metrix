// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The DarkCoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef ACTIVEMASTERNODE_H
#define ACTIVEMASTERNODE_H

#include "darksend.h"
#include "init.h"
#include "key.h"
#include "main.h"
#include "net.h"
#include "sync.h"
#include "uint256.h"
#include "wallet.h"
//! #include "primitives/transaction.h"

//! Responsible for activating the masternode and pinging the network
class CActiveMasternode
{
public:
    //! Initialized by init.cpp
    //! Keys for the main masternode
    CPubKey pubKeyMasternode;

    //! Initialized while registering masternode
    CTxIn vin;
    CService service;

    int status;
    std::string notCapableReason;

    CActiveMasternode()
    {
        status = MASTERNODE_NOT_PROCESSED;
    }

    void ManageStatus(); //! manage status of main masternode

    bool Dseep(std::string& errorMessage);                                                                      //! ping for main masternode
    bool Dseep(CTxIn vin, CService service, CKey key, CPubKey pubKey, std::string& retErrorMessage, bool stop); //! ping for any masternode

    bool StopMasterNode(std::string& errorMessage);                                                        //! stop main masternode
    bool StopMasterNode(std::string strService, std::string strKeyMasternode, std::string& errorMessage);  //! stop remote masternode
    bool StopMasterNode(CTxIn vin, CService service, CKey key, CPubKey pubKey, std::string& errorMessage); //! stop any masternode

    bool Register(std::string strService, std::string strKey, std::string txHash, std::string strOutputIndex, std::string& errorMessage);             //! register remote masternode
    bool Register(CTxIn vin, CService service, CKey key, CPubKey pubKey, CKey keyMasternode, CPubKey pubKeyMasternode, std::string& retErrorMessage); //! register any masternode

    //! get 1000Metrix input that can be used for the masternode
    bool GetMasterNodeVin(CTxIn& vin, CPubKey& pubkey, CKey& secretKey);
    bool GetMasterNodeVin(CTxIn& vin, CPubKey& pubkey, CKey& secretKey, std::string strTxHash, std::string strOutputIndex, bool includeLocked = false);
    vector<COutput> SelectCoinsMasternode(bool includeLocked = false);

    bool GetVinFromOutput(COutput out, CTxIn& vin, CPubKey& pubkey, CKey& secretKey);

    //! enable hot wallet mode (run a masternode with no funds)
    bool EnableHotColdMasterNode(CTxIn& vin, CService& addr);
};

#endif
