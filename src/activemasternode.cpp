// Copyright (c) 2009-2012 The Darkcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activemasternode.h"
#include "clientversion.h"
#include "main.h"
#include "protocol.h"

#include <boost/lexical_cast.hpp>

/**
 * Bootup the masternode, look for a 500 MRX input and register on the network
 */
void CActiveMasternode::ManageStatus()
{
    std::string errorMessage;

    if (!fMasterNode)
        return;

    if (fDebug)
        LogPrintf("CActiveMasternode::ManageStatus() - Begin\n");

    //! need correct adjusted time to send ping
    bool fIsInitialDownload = IsInitialBlockDownload();
    if (fIsInitialDownload) {
        status = MASTERNODE_SYNC_IN_PROCESS;
        LogPrintf("CActiveMasternode::ManageStatus() - Sync in progress. Must wait until sync is complete to start masternode.\n");
        return;
    }

    if (status == MASTERNODE_INPUT_TOO_NEW || status == MASTERNODE_NOT_CAPABLE || status == MASTERNODE_SYNC_IN_PROCESS) {
        status = MASTERNODE_NOT_PROCESSED;
    }

    if (status == MASTERNODE_NOT_PROCESSED) {
        if (strMasterNodeAddr.empty()) {
            if (!GetLocal(service)) {
                notCapableReason = "Can't detect external address. Please use the masternodeaddr configuration option.";
                status = MASTERNODE_NOT_CAPABLE;
                LogPrintf("CActiveMasternode::ManageStatus() - not capable: %s\n", notCapableReason);
                return;
            }

        } else {
            service = CService(strMasterNodeAddr);
        }

        LogPrintf("CActiveMasternode::ManageStatus() - Checking inbound connection to '%s'\n", service.ToString());

        /**
         * Metrix NOTE: There is no logical reason to restrict this to a specific port.  Its a peer, what difference does it make.
         *  if(service.GetPort() != 9999) {
         *       notCapableReason = "Invalid port: " + boost::lexical_cast<string>(service.GetPort()) + " -only 9999 is supported on mainnet.";
         *       status = MASTERNODE_NOT_CAPABLE;
         *       LogPrintf("CActiveMasternode::ManageStatus() - not capable: %s\n", notCapableReason);
         *       return;
         *   }
         */


        if (!ConnectNode((CAddress)service, service.ToString().c_str())) {
            notCapableReason = "Could not connect to " + service.ToString();
            status = MASTERNODE_NOT_CAPABLE;
            LogPrintf("CActiveMasternode::ManageStatus() - not capable: %s\n", notCapableReason);
            return;
        }


        if (pwalletMain->IsLocked(true)) {
            notCapableReason = "Wallet is locked.";
            status = MASTERNODE_NOT_CAPABLE;
            LogPrintf("CActiveMasternode::ManageStatus() - not capable: %s\n", notCapableReason);
            return;
        }

        //! Set defaults
        status = MASTERNODE_NOT_CAPABLE;
        notCapableReason = "Unknown. Check debug.log for more information.\n";

        //! Choose coins to use
        CPubKey pubKeyCollateralAddress;
        CKey keyCollateralAddress;

        //! Find possible candidates
        vector<COutput> possibleCoins = SelectCoinsMasternode();
        COutput* selectedOutput;

        BOOST_FOREACH (COutput& out, possibleCoins) {
            selectedOutput = &out;
            //! At this point we have a selected output, retrieve the associated info
            if (GetVinFromOutput(*selectedOutput, vin, pubKeyCollateralAddress, keyCollateralAddress)) {
                if (GetInputAge(vin) < MASTERNODE_MIN_CONFIRMATIONS) {
                    LogPrintf("CActiveMasternode::ManageStatus() - Input must have least %d confirmations - %d confirmations\n", MASTERNODE_MIN_CONFIRMATIONS, GetInputAge(vin));
                    status = MASTERNODE_INPUT_TOO_NEW;
                } else {
                    LogPrintf("CActiveMasternode::ManageStatus() - Is capable master node!\n");

                    status = MASTERNODE_IS_CAPABLE;
                    notCapableReason = "";

                    pwalletMain->LockCoin(vin.prevout);

                    //! send to all nodes
                    CPubKey pubKeyMasternode;
                    CKey keyMasternode;

                    if (!darkSendSigner.SetKey(strMasterNodePrivKey, errorMessage, keyMasternode, pubKeyMasternode)) {
                        LogPrintf("Register::ManageStatus() - Error upon calling SetKey: %s\n", errorMessage);
                    } else if (!Register(vin, service, keyCollateralAddress, pubKeyCollateralAddress, keyMasternode, pubKeyMasternode, errorMessage)) {
                        LogPrintf("CActiveMasternode::ManageStatus() - Error on Register: %s\n", errorMessage);
                    }

                    return;
                }
            }
        }

        LogPrintf("CActiveMasternode::ManageStatus() - Could not find suitable coins!\n");
    }

    //! send to all peers
    if (!Dseep(errorMessage)) {
        LogPrintf("CActiveMasternode::ManageStatus() - Error on Ping: %s", errorMessage);
    }
}

//! Send stop dseep to network for remote masternode
bool CActiveMasternode::StopMasterNode(std::string strService, std::string strKeyMasternode, std::string& errorMessage)
{
    CTxIn vin;
    CKey keyMasternode;
    CPubKey pubKeyMasternode;

    if (!darkSendSigner.SetKey(strKeyMasternode, errorMessage, keyMasternode, pubKeyMasternode)) {
        LogPrintf("CActiveMasternode::StopMasterNode() - Error: %s\n", errorMessage);
        return false;
    }

    return StopMasterNode(vin, CService(strService), keyMasternode, pubKeyMasternode, errorMessage);
}

//! Send stop dseep to network for main masternode
bool CActiveMasternode::StopMasterNode(std::string& errorMessage)
{
    if (status != MASTERNODE_IS_CAPABLE && status != MASTERNODE_REMOTELY_ENABLED) {
        errorMessage = "masternode is not in a running status";
        LogPrintf("CActiveMasternode::StopMasterNode() - Error: %s\n", errorMessage);
        return false;
    }

    status = MASTERNODE_STOPPED;

    CPubKey pubKeyMasternode;
    CKey keyMasternode;

    if (!darkSendSigner.SetKey(strMasterNodePrivKey, errorMessage, keyMasternode, pubKeyMasternode)) {
        LogPrintf("Register::ManageStatus() - Error upon calling SetKey: %s\n", errorMessage);
        return false;
    }

    return StopMasterNode(vin, service, keyMasternode, pubKeyMasternode, errorMessage);
}

//! Send stop dseep to network for any masternode
bool CActiveMasternode::StopMasterNode(CTxIn vin, CService service, CKey keyMasternode, CPubKey pubKeyMasternode, std::string& errorMessage)
{
    pwalletMain->UnlockCoin(vin.prevout);
    return Dseep(vin, service, keyMasternode, pubKeyMasternode, errorMessage, true);
}

bool CActiveMasternode::Dseep(std::string& errorMessage)
{
    if (status != MASTERNODE_IS_CAPABLE && status != MASTERNODE_REMOTELY_ENABLED) {
        errorMessage = "masternode is not in a running status";
        LogPrintf("CActiveMasternode::Dseep() - Error: %s\n", errorMessage);
        return false;
    }

    CPubKey pubKeyMasternode;
    CKey keyMasternode;

    if (!darkSendSigner.SetKey(strMasterNodePrivKey, errorMessage, keyMasternode, pubKeyMasternode)) {
        LogPrintf("Register::ManageStatus() - Error upon calling SetKey: %s\n", errorMessage);
        return false;
    }

    return Dseep(vin, service, keyMasternode, pubKeyMasternode, errorMessage, false);
}

bool CActiveMasternode::Dseep(CTxIn vin, CService service, CKey keyMasternode, CPubKey pubKeyMasternode, std::string& retErrorMessage, bool stop)
{
    std::string errorMessage;
    std::vector<unsigned char> vchMasterNodeSignature;
    std::string strMasterNodeSignMessage;
    int64_t masterNodeSignatureTime = GetAdjustedTime();

    std::string strMessage = service.ToString() + boost::lexical_cast<std::string>(masterNodeSignatureTime) + boost::lexical_cast<std::string>(stop);

    if (!darkSendSigner.SignMessage(strMessage, errorMessage, vchMasterNodeSignature, keyMasternode)) {
        retErrorMessage = "sign message failed: " + errorMessage;
        LogPrintf("CActiveMasternode::Dseep() - Error: %s\n", retErrorMessage);
        return false;
    }

    if (!darkSendSigner.VerifyMessage(pubKeyMasternode, vchMasterNodeSignature, strMessage, errorMessage)) {
        retErrorMessage = "Verify message failed: " + errorMessage;
        LogPrintf("CActiveMasternode::Dseep() - Error: %s\n", retErrorMessage);
        return false;
    }

    //! Update Last Seen timestamp in masternode list
    bool found = false;
    BOOST_FOREACH (CMasterNode& mn, vecMasternodes) {
        //! LogPrintf(" -- %s\n", mn.vin.ToString());
        if (mn.vin == vin) {
            found = true;
            mn.UpdateLastSeen();
        }
    }

    if (!found) {
        //! Seems like we are trying to send a ping while the masternode is not registered in the network
        retErrorMessage = "Darksend Masternode List doesn't include our masternode, Shutting down masternode pinging service! " + vin.ToString();
        LogPrintf("CActiveMasternode::Dseep() - Error: %s\n", retErrorMessage);
        status = MASTERNODE_NOT_CAPABLE;
        notCapableReason = retErrorMessage;
        return false;
    }

    //! send to all peers
    LogPrintf("CActiveMasternode::Dseep() - SendDarkSendElectionEntryPing vin = %s\n", vin.ToString());
    SendDarkSendElectionEntryPing(vin, vchMasterNodeSignature, masterNodeSignatureTime, stop);

    return true;
}

bool CActiveMasternode::Register(std::string strService, std::string strKeyMasternode, std::string txHash, std::string strOutputIndex, std::string& errorMessage)
{
    CTxIn vin;
    CPubKey pubKeyCollateralAddress;
    CKey keyCollateralAddress;
    CPubKey pubKeyMasternode;
    CKey keyMasternode;

    if (!darkSendSigner.SetKey(strKeyMasternode, errorMessage, keyMasternode, pubKeyMasternode)) {
        LogPrintf("CActiveMasternode::Register() - Error upon calling SetKey: %s\n", errorMessage);
        return false;
    }

    if (!GetMasterNodeVin(vin, pubKeyCollateralAddress, keyCollateralAddress, txHash, strOutputIndex, true)) {
        errorMessage = "could not allocate vin";
        LogPrintf("Register::Register() - Error: %s\n", errorMessage);
        return false;
    }
    return Register(vin, CService(strService), keyCollateralAddress, pubKeyCollateralAddress, keyMasternode, pubKeyMasternode, errorMessage);
}

bool CActiveMasternode::Register(CTxIn vin, CService service, CKey keyCollateralAddress, CPubKey pubKeyCollateralAddress, CKey keyMasternode, CPubKey pubKeyMasternode, std::string& retErrorMessage)
{
    std::string errorMessage;
    std::vector<unsigned char> vchMasterNodeSignature;
    std::string strMasterNodeSignMessage;
    int64_t masterNodeSignatureTime = GetAdjustedTime();

    std::string vchPubKey(pubKeyCollateralAddress.begin(), pubKeyCollateralAddress.end());
    std::string vchPubKey2(pubKeyMasternode.begin(), pubKeyMasternode.end());

    std::string strMessage = service.ToString() + boost::lexical_cast<std::string>(masterNodeSignatureTime) + vchPubKey + vchPubKey2 + boost::lexical_cast<std::string>(PROTOCOL_VERSION);

    if (!darkSendSigner.SignMessage(strMessage, errorMessage, vchMasterNodeSignature, keyCollateralAddress)) {
        retErrorMessage = "sign message failed: " + errorMessage;
        LogPrintf("CActiveMasternode::Register() - Error: %s\n", retErrorMessage);
        return false;
    }

    if (!darkSendSigner.VerifyMessage(pubKeyCollateralAddress, vchMasterNodeSignature, strMessage, errorMessage)) {
        retErrorMessage = "Verify message failed: " + errorMessage;
        LogPrintf("CActiveMasternode::Register() - Error: %s\n", retErrorMessage);
        return false;
    }

    bool found = false;
    BOOST_FOREACH (CMasterNode& mn, vecMasternodes)
        if (mn.vin == vin)
            found = true;

    if (!found) {
        // extract masternode collateral
        CAmount mnCollateral;
        darkSendSigner.IsVinAssociatedWithPubkey(vin, pubKeyCollateralAddress, &mnCollateral);

        LogPrintf("CActiveMasternode::Register() - Adding to masternode list service: %s - vin: %s\n", service.ToString(), vin.ToString());
        CMasterNode mn(service, vin, pubKeyCollateralAddress, vchMasterNodeSignature, masterNodeSignatureTime, pubKeyMasternode, PROTOCOL_VERSION, mnCollateral);
        mn.UpdateLastSeen(masterNodeSignatureTime);
        vecMasternodes.push_back(mn);
    }

    //! send to all peers
    LogPrintf("CActiveMasternode::Register() - SendDarkSendElectionEntry vin = %s\n", vin.ToString());
    SendDarkSendElectionEntry(vin, service, vchMasterNodeSignature, masterNodeSignatureTime, pubKeyCollateralAddress, pubKeyMasternode, -1, -1, masterNodeSignatureTime, PROTOCOL_VERSION);

    return true;
}

bool CActiveMasternode::GetMasterNodeVin(CTxIn& vin, CPubKey& pubkey, CKey& secretKey)
{
    return GetMasterNodeVin(vin, pubkey, secretKey, "", "");
}

bool CActiveMasternode::GetMasterNodeVin(CTxIn& vin, CPubKey& pubkey, CKey& secretKey, std::string strTxHash, std::string strOutputIndex, bool includeLocked)
{
    CScript pubScript;

    //! Find possible candidates
    vector<COutput> possibleCoins = SelectCoinsMasternode(includeLocked);
    COutput* selectedOutput;

    //! Find the vin
    if (!strTxHash.empty()) {
        //! Let's find it
        uint256 txHash(strTxHash);
        int outputIndex = boost::lexical_cast<int>(strOutputIndex);
        bool found = false;
        BOOST_FOREACH (COutput& out, possibleCoins) {
            if (out.tx->GetHash() == txHash && out.i == outputIndex) {
                selectedOutput = &out;
                found = true;
                break;
            }
        }
        if (!found) {
            LogPrintf("CActiveMasternode::GetMasterNodeVin - Could not locate valid vin\n");
            return false;
        }
    } else {
        //! No output specified,  Select the first one
        if (possibleCoins.size() > 0) {
            selectedOutput = &possibleCoins[0];
        } else {
            LogPrintf("CActiveMasternode::GetMasterNodeVin - Could not locate specified vin from possible list\n");
            return false;
        }
    }

    //! At this point we have a selected output, retrieve the associated info
    return GetVinFromOutput(*selectedOutput, vin, pubkey, secretKey);
}


//! Extract masternode vin information from output
bool CActiveMasternode::GetVinFromOutput(COutput out, CTxIn& vin, CPubKey& pubkey, CKey& secretKey)
{
    CScript pubScript;

    vin = CTxIn(out.tx->GetHash(), out.i);
    pubScript = out.tx->vout[out.i].scriptPubKey; //! the inputs PubKey

    CTxDestination address1;
    ExtractDestination(pubScript, address1);
    CBitcoinAddress address2(address1);

    CKeyID keyID;
    if (!address2.GetKeyID(keyID)) {
        LogPrintf("CActiveMasternode::GetMasterNodeVin - Address does not refer to a key\n");
        return false;
    }

    if (!pwalletMain->GetKey(keyID, secretKey)) {
        LogPrintf("CActiveMasternode::GetMasterNodeVin - Private key for address is not known\n");
        return false;
    }

    pubkey = secretKey.GetPubKey();
    return true;
}

//! get all possible outputs for running masternode
vector<COutput> CActiveMasternode::SelectCoinsMasternode(bool includeLocked)
{
    vector<COutput> vCoins;
    vector<COutput> filteredCoins;

    /**
     * Retrieve all possible outputs
     * Metrix - start-many and start-alias should include locked coins as nodes in
     * the masternode.conf file coins are looked on startup. otherwise it wouldn't
     * be possible to start the node remotely as it would alywas fail to find vin
     */
    pwalletMain->AvailableCoins(vCoins, true, NULL, false, ALL_COINS, false, (int)1, includeLocked);

    //! Filter
    BOOST_FOREACH (const COutput& out, vCoins) {
        if (IsValidMasternodeCollateral(out.tx->vout[out.i].nValue, chainActive.Tip()))
            filteredCoins.push_back(out);
    }

    return filteredCoins;
}

//! when starting a masternode, this can enable to run as a hot wallet with no funds
bool CActiveMasternode::EnableHotColdMasterNode(CTxIn& newVin, CService& newService)
{
    if (!fMasterNode)
        return false;

    status = MASTERNODE_REMOTELY_ENABLED;

    //! The values below are needed for signing dseep messages going forward
    this->vin = newVin;
    this->service = newService;

    LogPrintf("CActiveMasternode::EnableHotColdMasterNode() - Enabled! You may shut down the cold daemon.\n");

    return true;
}