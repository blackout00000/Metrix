// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "rpcserver.h"

#include "base58.h"
#include "db.h"
#include "init.h"
#include "main.h"
#include "sync.h"
#include "ui_interface.h"
#include "util.h"
#include "wallet.h"
#ifdef ENABLE_WALLET
#include "wallet.h"
#endif

#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ip/v6_only.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/bind.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/foreach.hpp>
#include <boost/iostreams/concepts.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/thread.hpp>
#include <list>

#include "univalue/univalue.h"

using namespace boost;
using namespace boost::asio;
using namespace std;

static std::string strRPCUserColonPass;

static bool fRPCInWarmup = true;
static std::string rpcWarmupStatus("RPC server started");
static CCriticalSection cs_rpcWarmup;
static bool fRPCRunning = false;

//! These are created by StartRPCThreads, destroyed in StopRPCThreads
static asio::io_service* rpc_io_service = NULL;
static map<string, boost::shared_ptr<deadline_timer> > deadlineTimers;
static ssl::context* rpc_ssl_context = NULL;
static boost::thread_group* rpc_worker_group = NULL;
static std::vector<CSubNet> rpc_allow_subnets; //! List of subnets to allow RPC connections from
static std::vector<boost::shared_ptr<ip::tcp::acceptor> > rpc_acceptors;

void RPCTypeCheck(const UniValue& params,
                  const list<UniValue::VType>& typesExpected,
                  bool fAllowNull)
{
    unsigned int i = 0;
    BOOST_FOREACH (UniValue::VType t, typesExpected) {
        if (params.size() <= i)
            break;

        const UniValue& v = params[i];
        if (!((v.type() == t) || (fAllowNull && (v.isNull()))))
        {
            string err = strprintf("Expected type %s, got %s",
                                   uvTypeName(t), uvTypeName(v.type()));
            throw JSONRPCError(RPC_TYPE_ERROR, err);
        }
        i++;
    }
}

void RPCTypeCheckObj(const UniValue& o,
                  const map<string, UniValue::VType>& typesExpected,
                  bool fAllowNull)
{
    BOOST_FOREACH(const PAIRTYPE(string, UniValue::VType)& t, typesExpected)
    {
        const UniValue& v = find_value(o, t.first);
        if (!fAllowNull && v.isNull())
            throw JSONRPCError(RPC_TYPE_ERROR, strprintf("Missing %s", t.first));

        if (!((v.type() == t.second) || (fAllowNull && (v.isNull()))))
        {
            string err = strprintf("Expected type %s for %s, got %s",
                                   uvTypeName(t.second), t.first, uvTypeName(v.type()));
            throw JSONRPCError(RPC_TYPE_ERROR, err);
        }
    }
}

static inline int64_t roundint64(double d)
{
    return (int64_t)(d > 0 ? d + 0.5 : d - 0.5);
}

CAmount AmountFromValue(const UniValue& value)
{
    double dAmount = value.get_real();
    if (dAmount <= 0.0 || dAmount > MAX_MONEY)
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount");
    CAmount nAmount = roundint64(dAmount * COIN);
    if (!MoneyRange(nAmount))
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount");
    return nAmount;
}

UniValue ValueFromAmount(const CAmount& amount)
{
    return (double)amount / (double)COIN;
}


/**
* Utilities: convert hex-encoded Values
* (throws error if not hex).
*/
uint256 ParseHashV(const UniValue& v, string strName)
{
    string strHex;
    if (v.isStr())
        strHex = v.get_str();
    if (!IsHex(strHex)) //! Note: IsHex("") is false
        throw JSONRPCError(RPC_INVALID_PARAMETER, strName + " must be hexadecimal string (not '" + strHex + "')");
    uint256 result;
    result.SetHex(strHex);
    return result;
}

uint256 ParseHashO(const UniValue& o, string strKey)
{
    return ParseHashV(find_value(o, strKey), strKey);
}

vector<unsigned char> ParseHexV(const UniValue& v, string strName)
{
    string strHex;
    if (v.isStr())
        strHex = v.get_str();
    if (!IsHex(strHex))
        throw JSONRPCError(RPC_INVALID_PARAMETER, strName + " must be hexadecimal string (not '" + strHex + "')");
    return ParseHex(strHex);
}

vector<unsigned char> ParseHexO(const UniValue& o, string strKey)
{
    return ParseHexV(find_value(o, strKey), strKey);
}


/**
* Note: This interface may still be subject to change.
*/

string CRPCTable::help(string strCommand) const
{
    string strRet;
    string category;
    set<rpcfn_type> setDone;
    vector<pair<string, const CRPCCommand*> > vCommands;

    for (map<string, const CRPCCommand*>::const_iterator mi = mapCommands.begin(); mi != mapCommands.end(); ++mi)
        vCommands.push_back(make_pair(mi->second->category + mi->first, mi->second));
    sort(vCommands.begin(), vCommands.end());

    BOOST_FOREACH (const PAIRTYPE(string, const CRPCCommand*) & command, vCommands) {
        const CRPCCommand* pcmd = command.second;
        string strMethod = pcmd->name;
        //! We already filter duplicates, but these deprecated screw up the sort order
        if (strMethod.find("label") != string::npos)
            continue;
        if ((strCommand != "" || pcmd->category == "hidden") && strMethod != strCommand)
            continue;
#ifdef ENABLE_WALLET
        if (pcmd->reqWallet && !pwalletMain)
            continue;
#endif

        try {
            UniValue params;
            rpcfn_type pfn = pcmd->actor;
            if (setDone.insert(pfn).second)
                (*pfn)(params, true);
        } catch (std::exception& e) {
            //! Help text is returned in an exception
            string strHelp = string(e.what());
            if (strCommand == "") {
                if (strHelp.find('\n') != string::npos)
                    strHelp = strHelp.substr(0, strHelp.find('\n'));

                if (category != pcmd->category) {
                    if (!category.empty())
                        strRet += "\n";
                    category = pcmd->category;
                    string firstLetter = category.substr(0, 1);
                    boost::to_upper(firstLetter);
                    strRet += "== " + firstLetter + category.substr(1) + " ==\n";
                }
            }
            strRet += strHelp + "\n";
        }
    }
    if (strRet == "")
        strRet = strprintf("help: unknown command: %s\n", strCommand);
    strRet = strRet.substr(0, strRet.size() - 1);
    return strRet;
}

UniValue help(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "help ( \"command\" )\n"
            "\nList all commands, or get help for a specified command.\n"
            "\nArguments:\n"
            "1. \"command\"     (string, optional) The command to get help on\n"
            "\nResult:\n"
            "\"text\"     (string) The help text\n");

    string strCommand;
    if (params.size() > 0)
        strCommand = params[0].get_str();

    return tableRPC.help(strCommand);
}


UniValue stop(const UniValue& params, bool fHelp)
{
    //! Accept the deprecated and ignored 'detach' boolean argument
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "stop\n"
            "\nStop Metrix server.");
    //! Shutdown will take long enough that the response should get back
    StartShutdown();
    return "Metrix server stopping";
}


/**
* Call Table
*/


static const CRPCCommand vRPCCommands[] =
    {
        //!  category              name                      actor (function)         okSafeMode threadSafe reqWallet
        //!  --------------------- ------------------------  -----------------------  ---------- ---------- ---------
        /* Overall control/query calls */
        {"control", "getinfo", &getinfo, true, false, false}, /* uses wallet if enabled */
        {"control", "help", &help, true, true, false},
        {"control", "stop", &stop, true, true, false},

        /* P2P networking */
        {"network", "getnetworkinfo", &getnetworkinfo, true, false, false},
        {"network", "addnode", &addnode, true, true, false},
        {"network", "getaddednodeinfo", &getaddednodeinfo, true, true, false},
        {"network", "getconnectioncount", &getconnectioncount, true, false, false},
        {"network", "getnettotals", &getnettotals, true, true, false},
        {"network", "getpeerinfo", &getpeerinfo, true, false, false},
        {"network", "ping", &ping, true, false, false},

        /* Block chain and UTXO */
        {"blockchain", "getblockchaininfo", &getblockchaininfo, true, false, false},
        {"blockchain", "getbestblockhash", &getbestblockhash, true, false, false},
        {"blockchain", "getblockcount", &getblockcount, true, false, false},
        {"blockchain", "getblock", &getblock, true, false, false},
        {"blockchain", "getblockbynumber", &getblockbynumber, false, false, false},
        {"blockchain", "getblockhash", &getblockhash, true, false, false},
        {"blockchain", "getchaintips", &getchaintips, true, false, false},
        {"blockchain", "getdifficulty", &getdifficulty, true, false, false},
        {"blockchain", "getmempoolinfo", &getmempoolinfo, true, true, false},
        {"blockchain", "getrawmempool", &getrawmempool, true, false, false},
        {"blockchain", "gettxout", &gettxout, true, false, false},
        {"blockchain", "gettxoutsetinfo", &gettxoutsetinfo, true, false, false},

        /* Staking */
        {"staking", "getblocktemplate", &getblocktemplate, true, false, false},
        {"staking", "getmininginfo", &getmininginfo, true, false, false},
        {"staking", "getstakinginfo", &getstakinginfo, true, false, false},
        {"staking", "prioritisetransaction", &prioritisetransaction, true, false, false},
        {"staking", "setstakesplitthreshold", &setstakesplitthreshold, true, false, false},

        /* Raw transactions */
        {"rawtransactions", "createrawtransaction", &createrawtransaction, true, false, false},
        {"rawtransactions", "decoderawtransaction", &decoderawtransaction, true, false, false},
        {"rawtransactions", "decodescript", &decodescript, true, false, false},
        {"rawtransactions", "getrawtransaction", &getrawtransaction, true, false, false},
        {"rawtransactions", "searchrawtransactions", &searchrawtransactions, false, false, false},
        {"rawtransactions", "sendrawtransaction", &sendrawtransaction, false, false, false},
        {"rawtransactions", "signrawtransaction", &signrawtransaction, false, false, false}, /* uses wallet if enabled */

        /* Utility functions */
        {"util", "createmultisig", &createmultisig, false, false, true},
        {"util", "validateaddress", &validateaddress, true, false, false},
        {"util", "validatepubkey", &validatepubkey, true, false, false},
        {"util", "verifymessage", &verifymessage, true, false, false},
        {"util", "estimatefee", &estimatefee, true, true, false },
        {"util", "estimatepriority", &estimatepriority, true, true, false },

        /* Not shown in help */
        {"hidden", "invalidateblock", &invalidateblock, true, true, false },
        {"hidden", "reconsiderblock", &reconsiderblock, true, true, false },

        /* Dark features */
        {"dark", "darksend", &darksend, false, false, true},
        {"dark", "spork", &spork, true, false, false},
        {"dark", "masternode", &masternode, true, false, true},
        {"dark", "keepass", &keepass, false, false, true},

#ifdef ENABLE_WALLET
        /* Wallet */
        {"wallet", "addmultisigaddress", &addmultisigaddress, true, false, true},
        {"wallet", "addredeemscript", &addredeemscript, false, false, true},
        {"wallet", "addressbookadd", &addressbookadd, true, false, true},
        {"wallet", "addressbookremove", &addressbookremove, true, false, true},
        {"wallet", "backupwallet", &backupwallet, true, false, true},
        {"wallet", "checkkernel", &checkkernel, true, false, true},
        {"wallet", "dumpprivkey", &dumpprivkey, false, false, true},
        {"wallet", "dumpwallet", &dumpwallet, true, false, true},
        {"wallet", "encryptwallet", &encryptwallet, true, false, true},
        {"wallet", "getaccountaddress", &getaccountaddress, true, false, true},
        {"wallet", "getaccount", &getaccount, true, false, true},
        {"wallet", "getaddressesbyaccount", &getaddressesbyaccount, true, false, true},
        {"wallet", "getbalance", &getbalance, false, false, true},
        {"wallet", "getunconfirmedbalance", &getunconfirmedbalance, false, false, true},
        {"wallet", "getnewaddress", &getnewaddress, true, false, true},
        {"wallet", "getnewstealthaddress", &getnewstealthaddress, false, false, true},
        {"wallet", "getreceivedbyaccount", &getreceivedbyaccount, false, false, true},
        {"wallet", "getreceivedbyaddress", &getreceivedbyaddress, false, false, true},
        {"wallet", "getstakesubsidy", &getstakesubsidy, true, true, false},
        {"wallet", "getsubsidy", &getsubsidy, true, true, false},
        {"wallet", "gettransaction", &gettransaction, false, false, true},
        {"wallet", "getwalletinfo", &getwalletinfo, false, false, true},
        {"wallet", "importprivkey", &importprivkey, true, false, true},
        {"wallet", "importstealthaddress", &importstealthaddress, true, false, true},
        {"wallet", "importwallet", &importwallet, true, false, true},
        {"wallet", "importaddress", &importaddress, true, false, true},
        {"wallet", "keypoolrefill", &keypoolrefill, true, false, true},
        {"wallet", "listaccounts", &listaccounts, false, false, true},
        {"wallet", "listaddressbook", &listaddressbook, false, false, true},
        {"wallet", "listaddressgroupings", &listaddressgroupings, false, false, true},
        {"wallet", "listlockunspent", &listlockunspent, false, false, false},
        {"wallet", "listreceivedbyaccount", &listreceivedbyaccount, false, false, true},
        {"wallet", "listreceivedbyaddress", &listreceivedbyaddress, false, false, true},
        {"wallet", "listsinceblock", &listsinceblock, false, false, true},
        {"wallet", "liststealthaddresses", &liststealthaddresses, false, false, true},
        {"wallet", "listtransactions", &listtransactions, false, false, true},
        {"wallet", "listunspent", &listunspent, false, false, true},
        {"wallet", "lockunspent", &lockunspent, true, false, false},
        {"wallet", "makekeypair", &makekeypair, false, true, false},
        {"wallet", "move", &movecmd, false, false, true},
        {"wallet", "resendtx", &resendtx, false, true, true},
        {"wallet", "reservebalance", &reservebalance, false, true, true},
        {"wallet", "sendalert", &sendalert, false, false, false},
        {"wallet", "sendfrom", &sendfrom, false, false, true},
        {"wallet", "sendmany", &sendmany, false, false, true},
        {"wallet", "sendtoaddress", &sendtoaddress, false, false, true},
        {"wallet", "sendtostealthaddress", &sendtostealthaddress, false, false, true},
        {"wallet", "setaccount", &setaccount, true, false, true},
        {"wallet", "settxfee", &settxfee, true, false, true},
        {"wallet", "signmessage", &signmessage, true, false, true},
        {"wallet", "walletlock", &walletlock, true, false, true},
        {"wallet", "walletpassphrasechange", &walletpassphrasechange, true, false, true},
        {"wallet", "walletpassphrase", &walletpassphrase, true, false, true},
        {"wallet", "getnewpubkey", &getnewpubkey, true, false, true},
#endif
};

CRPCTable::CRPCTable()
{
    unsigned int vcidx;
    for (vcidx = 0; vcidx < (sizeof(vRPCCommands) / sizeof(vRPCCommands[0])); vcidx++) {
        const CRPCCommand* pcmd;

        pcmd = &vRPCCommands[vcidx];
        mapCommands[pcmd->name] = pcmd;
    }
}

const CRPCCommand* CRPCTable::operator[](string name) const
{
    map<string, const CRPCCommand*>::const_iterator it = mapCommands.find(name);
    if (it == mapCommands.end())
        return NULL;
    return (*it).second;
}


bool HTTPAuthorized(map<string, string>& mapHeaders)
{
    string strAuth = mapHeaders["authorization"];
    if (strAuth.substr(0, 6) != "Basic ")
        return false;
    string strUserPass64 = strAuth.substr(6);
    boost::trim(strUserPass64);
    string strUserPass = DecodeBase64(strUserPass64);
    return TimingResistantEqual(strUserPass, strRPCUserColonPass);
}

void ErrorReply(std::ostream& stream, const UniValue& objError, const UniValue& id)
{
    //! Send error reply from json-rpc error object
    int nStatus = HTTP_INTERNAL_SERVER_ERROR;
    int code = find_value(objError, "code").get_int();
    if (code == RPC_INVALID_REQUEST)
        nStatus = HTTP_BAD_REQUEST;
    else if (code == RPC_METHOD_NOT_FOUND)
        nStatus = HTTP_NOT_FOUND;
    string strReply = JSONRPCReply(NullUniValue, objError, id);
    stream << HTTPReply(nStatus, strReply, false) << std::flush;
}

CNetAddr BoostAsioToCNetAddr(boost::asio::ip::address address)
{
    CNetAddr netaddr;
    //! Make sure that IPv4-compatible and IPv4-mapped IPv6 addresses are treated as IPv4 addresses
    if (address.is_v6() && (address.to_v6().is_v4_compatible() || address.to_v6().is_v4_mapped()))
        address = address.to_v6().to_v4();

    if (address.is_v4()) {
        boost::asio::ip::address_v4::bytes_type bytes = address.to_v4().to_bytes();
        netaddr.SetRaw(NET_IPV4, &bytes[0]);
    } else {
        boost::asio::ip::address_v6::bytes_type bytes = address.to_v6().to_bytes();
        netaddr.SetRaw(NET_IPV6, &bytes[0]);
    }
    return netaddr;
}

bool ClientAllowed(const boost::asio::ip::address& address)
{
    CNetAddr netaddr = BoostAsioToCNetAddr(address);
    BOOST_FOREACH (const CSubNet& subnet, rpc_allow_subnets)
        if (subnet.Match(netaddr))
            return true;
    return false;
}

template <typename Protocol>
class AcceptedConnectionImpl : public AcceptedConnection
{
public:
    AcceptedConnectionImpl(
        asio::io_service& io_service,
        ssl::context& context,
        bool fUseSSL) : sslStream(io_service, context),
                        _d(sslStream, fUseSSL),
                        _stream(_d)
    {
    }

    virtual std::iostream& stream()
    {
        return _stream;
    }

    virtual std::string peer_address_to_string() const
    {
        return peer.address().to_string();
    }

    virtual void close()
    {
        _stream.close();
    }

    typename Protocol::endpoint peer;
    asio::ssl::stream<typename Protocol::socket> sslStream;

private:
    SSLIOStreamDevice<Protocol> _d;
    iostreams::stream<SSLIOStreamDevice<Protocol> > _stream;
};

void ServiceConnection(AcceptedConnection* conn);

//! Forward declaration required for RPCListen
template <typename Protocol, typename SocketAcceptorService>
static void RPCAcceptHandler(boost::shared_ptr<basic_socket_acceptor<Protocol, SocketAcceptorService> > acceptor,
                             ssl::context& context,
                             bool fUseSSL,
                             boost::shared_ptr<AcceptedConnection> conn,
                             const boost::system::error_code& error);

/**
 * Sets up I/O resources to accept and handle a new connection.
 */
template <typename Protocol, typename SocketAcceptorService>
static void RPCListen(boost::shared_ptr<basic_socket_acceptor<Protocol, SocketAcceptorService> > acceptor,
                      ssl::context& context,
                      const bool fUseSSL)
{
    //! Accept connection
    boost::shared_ptr<AcceptedConnectionImpl<Protocol> > conn(new AcceptedConnectionImpl<Protocol>(acceptor->get_io_service(), context, fUseSSL));

    acceptor->async_accept(
        conn->sslStream.lowest_layer(),
        conn->peer,
        boost::bind(&RPCAcceptHandler<Protocol, SocketAcceptorService>,
                    acceptor,
                    boost::ref(context),
                    fUseSSL,
                    conn,
                    _1));
}


/**
 * Accept and handle incoming connection.
 */
template <typename Protocol, typename SocketAcceptorService>
static void RPCAcceptHandler(boost::shared_ptr<basic_socket_acceptor<Protocol, SocketAcceptorService> > acceptor,
                             ssl::context& context,
                             const bool fUseSSL,
                             boost::shared_ptr<AcceptedConnection> conn,
                             const boost::system::error_code& error)
{
    //! Immediately start accepting new connections, except when we're cancelled or our socket is closed.
    if (error != asio::error::operation_aborted && acceptor->is_open())
        RPCListen(acceptor, context, fUseSSL);

    AcceptedConnectionImpl<ip::tcp>* tcp_conn = dynamic_cast<AcceptedConnectionImpl<ip::tcp>*>(conn.get());

    //! TODO: Actually handle errors
    if (error) {
        //! TODO: Actually handle errors
        LogPrintf("%s: Error: %s\n", __func__, error.message());
    }
    /**
    * Restrict callers by IP.  It is important to
    * do this before starting client thread, to filter out
    * certain DoS and misbehaving clients.
    */
    else if (tcp_conn && !ClientAllowed(tcp_conn->peer.address())) {
        //! Only send a 403 if we're not using SSL to prevent a DoS during the SSL handshake.
        if (!fUseSSL)
            conn->stream() << HTTPError(HTTP_FORBIDDEN, false) << std::flush;
        conn->close();
    } else {
        ServiceConnection(conn.get());
        conn->close();
    }
}

static ip::tcp::endpoint ParseEndpoint(const std::string& strEndpoint, int defaultPort)
{
    std::string addr;
    int port = defaultPort;
    SplitHostPort(strEndpoint, port, addr);
    return ip::tcp::endpoint(asio::ip::address::from_string(addr), port);
}

void StartRPCThreads()
{
    rpc_allow_subnets.clear();
    rpc_allow_subnets.push_back(CSubNet("127.0.0.0/8")); //! always allow IPv4 local subnet
    rpc_allow_subnets.push_back(CSubNet("::1"));         //! always allow IPv6 localhost
    if (mapMultiArgs.count("-rpcallowip")) {
        const vector<string>& vAllow = mapMultiArgs["-rpcallowip"];
        BOOST_FOREACH (string strAllow, vAllow) {
            CSubNet subnet(strAllow);
            if (!subnet.IsValid()) {
                uiInterface.ThreadSafeMessageBox(
                    strprintf("Invalid -rpcallowip subnet specification: %s. Valid are a single IP (e.g. 1.2.3.4), a network/netmask (e.g. 1.2.3.4/255.255.255.0) or a network/CIDR (e.g. 1.2.3.4/24).", strAllow),
                    "", CClientUIInterface::MSG_ERROR);
                StartShutdown();
                return;
            }
            rpc_allow_subnets.push_back(subnet);
        }
    }
    std::string strAllowed;
    BOOST_FOREACH (const CSubNet& subnet, rpc_allow_subnets)
        strAllowed += subnet.ToString() + " ";
    LogPrint("rpc", "Allowing RPC connections from: %s\n", strAllowed);

    strRPCUserColonPass = mapArgs["-rpcuser"] + ":" + mapArgs["-rpcpassword"];
    if (((mapArgs["-rpcpassword"] == "") ||
         (mapArgs["-rpcuser"] == mapArgs["-rpcpassword"])) &&
        Params().RequireRPCPassword()) {
        unsigned char rand_pwd[32];
        GetRandBytes(rand_pwd, 32);
        string strWhatAmI = "To use metrixd";
        if (mapArgs.count("-server"))
            strWhatAmI = strprintf(_("To use the %s option"), "\"-server\"");
        else if (mapArgs.count("-daemon"))
            strWhatAmI = strprintf(_("To use the %s option"), "\"-daemon\"");
        uiInterface.ThreadSafeMessageBox(strprintf(
                                             _("%s, you must set a rpcpassword in the configuration file:\n"
                                               "%s\n"
                                               "It is recommended you use the following random password:\n"
                                               "rpcuser=Metrixrpc\n"
                                               "rpcpassword=%s\n"
                                               "(you do not need to remember this password)\n"
                                               "The username and password MUST NOT be the same.\n"
                                               "If the file does not exist, create it with owner-readable-only file permissions.\n"
                                               "It is also recommended to set alertnotify so you are notified of problems;\n"
                                               "for example: alertnotify=echo %%s | mail -s \"Metrix Alert\" admin@foo.com\n"),
                                             strWhatAmI,
                                             GetConfigFile().string(),
                                             EncodeBase58(&rand_pwd[0], &rand_pwd[0] + 32)),
                                         "", CClientUIInterface::MSG_ERROR);
        StartShutdown();
        return;
    }

    assert(rpc_io_service == NULL);
    rpc_io_service = new asio::io_service();
    rpc_ssl_context = new ssl::context(*rpc_io_service, ssl::context::sslv23);

    const bool fUseSSL = GetBoolArg("-rpcssl", false);

    if (fUseSSL) {
        rpc_ssl_context->set_options(ssl::context::no_sslv2 | ssl::context::no_sslv3);

        filesystem::path pathCertFile(GetArg("-rpcsslcertificatechainfile", "server.cert"));
        if (!pathCertFile.is_complete())
            pathCertFile = filesystem::path(GetDataDir()) / pathCertFile;
        if (filesystem::exists(pathCertFile))
            rpc_ssl_context->use_certificate_chain_file(pathCertFile.string());
        else
            LogPrintf("ThreadRPCServer ERROR: missing server certificate file %s\n", pathCertFile.string());

        filesystem::path pathPKFile(GetArg("-rpcsslprivatekeyfile", "server.pem"));
        if (!pathPKFile.is_complete())
            pathPKFile = filesystem::path(GetDataDir()) / pathPKFile;
        if (filesystem::exists(pathPKFile))
            rpc_ssl_context->use_private_key_file(pathPKFile.string(), ssl::context::pem);
        else
            LogPrintf("ThreadRPCServer ERROR: missing server private key file %s\n", pathPKFile.string());

        string strCiphers = GetArg("-rpcsslciphers", "TLSv1.2+HIGH:TLSv1+HIGH:!SSLv2:!aNULL:!eNULL:!3DES:@STRENGTH");
        SSL_CTX_set_cipher_list(rpc_ssl_context->impl(), strCiphers.c_str());
    }

    std::vector<ip::tcp::endpoint> vEndpoints;
    bool bBindAny = false;
    int defaultPort = GetArg("-rpcport", BaseParams().RPCPort());
    if (!mapArgs.count("-rpcallowip")) //! Default to loopback if not allowing external IPs
    {
        vEndpoints.push_back(ip::tcp::endpoint(asio::ip::address_v6::loopback(), defaultPort));
        vEndpoints.push_back(ip::tcp::endpoint(asio::ip::address_v4::loopback(), defaultPort));
        if (mapArgs.count("-rpcbind")) {
            LogPrintf("WARNING: option -rpcbind was ignored because -rpcallowip was not specified, refusing to allow everyone to connect\n");
        }
    } else if (mapArgs.count("-rpcbind")) //! Specific bind address
    {
        BOOST_FOREACH (const std::string& addr, mapMultiArgs["-rpcbind"]) {
            try {
                vEndpoints.push_back(ParseEndpoint(addr, defaultPort));
            } catch (const boost::system::system_error&) {
                uiInterface.ThreadSafeMessageBox(
                    strprintf(_("Could not parse -rpcbind value %s as network address"), addr),
                    "", CClientUIInterface::MSG_ERROR);
                StartShutdown();
                return;
            }
        }
    } else { //! No specific bind address specified, bind to any
        vEndpoints.push_back(ip::tcp::endpoint(asio::ip::address_v6::any(), defaultPort));
        vEndpoints.push_back(ip::tcp::endpoint(asio::ip::address_v4::any(), defaultPort));
        //! Prefer making the socket dual IPv6/IPv4 instead of binding
        //! to both addresses seperately.
        bBindAny = true;
    }

    bool fListening = false;
    std::string strerr;
    std::string straddress;
    BOOST_FOREACH (const ip::tcp::endpoint& endpoint, vEndpoints) {
        try {
            asio::ip::address bindAddress = endpoint.address();
            straddress = bindAddress.to_string();
            LogPrintf("Binding RPC on address %s port %i (IPv4+IPv6 bind any: %i)\n", straddress, endpoint.port(), bBindAny);
            boost::system::error_code v6_only_error;
            boost::shared_ptr<ip::tcp::acceptor> acceptor(new ip::tcp::acceptor(*rpc_io_service));
            acceptor->open(endpoint.protocol());
            acceptor->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));

            //! Try making the socket dual IPv6/IPv4 when listening on the IPv6 "any" address
            acceptor->set_option(boost::asio::ip::v6_only(
                                     !bBindAny || bindAddress != asio::ip::address_v6::any()),
                                 v6_only_error);

            acceptor->bind(endpoint);
            acceptor->listen(socket_base::max_connections);

            RPCListen(acceptor, *rpc_ssl_context, fUseSSL);

            rpc_acceptors.push_back(acceptor);
            fListening = true;

            //! If dual IPv6/IPv4 bind succesful, skip binding to IPv4 separately
            if (bBindAny && bindAddress == asio::ip::address_v6::any() && !v6_only_error)
                break;
        } catch (boost::system::system_error& e) {
            LogPrintf("ERROR: Binding RPC on address %s port %i failed: %s\n", straddress, endpoint.port(), e.what());
            strerr = strprintf(_("An error occurred while setting up the RPC address %s port %u for listening: %s"), straddress, endpoint.port(), e.what());
        }
    }

    if (!fListening) {
        uiInterface.ThreadSafeMessageBox(strerr, "", CClientUIInterface::MSG_ERROR);
        StartShutdown();
        return;
    }

    rpc_worker_group = new boost::thread_group();
    for (int i = 0; i < GetArg("-rpcthreads", 4); i++)
        rpc_worker_group->create_thread(boost::bind(&asio::io_service::run, rpc_io_service));
    fRPCRunning = true;
}

void StopRPCThreads()
{
    if (rpc_io_service == NULL)
        return;
    //! Set this to false first, so that longpolling loops will exit when woken up
    fRPCRunning = false;
    /**
    * First, cancel all timers and acceptors
    * This is not done automatically by ->stop(), and in some cases the destructor of
    * asio::io_service can hang if this is skipped.
    */
    boost::system::error_code ec;
    BOOST_FOREACH (const boost::shared_ptr<ip::tcp::acceptor>& acceptor, rpc_acceptors) {
        acceptor->cancel(ec);
        if (ec)
            LogPrintf("%s: Warning: %s when cancelling acceptor", __func__, ec.message());
    }
    rpc_acceptors.clear();
    BOOST_FOREACH (const PAIRTYPE(std::string, boost::shared_ptr<deadline_timer>) & timer, deadlineTimers) {
        timer.second->cancel(ec);
        if (ec)
            LogPrintf("%s: Warning: %s when cancelling timer", __func__, ec.message());
    }
    deadlineTimers.clear();

    rpc_io_service->stop();
    cvBlockChange.notify_all();
    if (rpc_worker_group != NULL)
        rpc_worker_group->join_all();
    delete rpc_worker_group;
    rpc_worker_group = NULL;
    delete rpc_ssl_context;
    rpc_ssl_context = NULL;
    delete rpc_io_service;
    rpc_io_service = NULL;
}

bool IsRPCRunning()
{
    return fRPCRunning;
}

void SetRPCWarmupStatus(const std::string& newStatus)
{
    LOCK(cs_rpcWarmup);
    rpcWarmupStatus = newStatus;
}
void SetRPCWarmupFinished()
{
    LOCK(cs_rpcWarmup);
    assert(fRPCInWarmup);
    fRPCInWarmup = false;
}

bool RPCIsInWarmup(std::string *outStatus)
{
    LOCK(cs_rpcWarmup);
    if (outStatus)
        *outStatus = rpcWarmupStatus;
    return fRPCInWarmup;
}

void RPCRunHandler(const boost::system::error_code& err, boost::function<void(void)> func)
{
    if (!err)
        func();
}

void RPCRunLater(const std::string& name, boost::function<void(void)> func, int64_t nSeconds)
{
    assert(rpc_io_service != NULL);

    if (deadlineTimers.count(name) == 0) {
        deadlineTimers.insert(make_pair(name,
                                        boost::shared_ptr<deadline_timer>(new deadline_timer(*rpc_io_service))));
    }
    deadlineTimers[name]->expires_from_now(posix_time::seconds(nSeconds));
    deadlineTimers[name]->async_wait(boost::bind(RPCRunHandler, _1, func));
}

class JSONRequest
{
public:
    UniValue id;
    string strMethod;
    UniValue params;

    JSONRequest() { id = NullUniValue; }
    void parse(const UniValue& valRequest);
};

void JSONRequest::parse(const UniValue& valRequest)
{
    //! Parse request
    if (!valRequest.isObject())
        throw JSONRPCError(RPC_INVALID_REQUEST, "Invalid Request object");
    const UniValue& request = valRequest.get_obj();

    //! Parse id now so errors from here on will have the id
    id = find_value(request, "id");

    //! Parse method
    UniValue valMethod = find_value(request, "method");
    if (valMethod.isNull())
        throw JSONRPCError(RPC_INVALID_REQUEST, "Missing method");
    if (!valMethod.isStr())
        throw JSONRPCError(RPC_INVALID_REQUEST, "Method must be a string");
    strMethod = valMethod.get_str();
    if (strMethod != "getblocktemplate")
        LogPrint("rpc", "ThreadRPCServer method=%s\n", SanitizeString(strMethod));

    //! Parse params
    UniValue valParams = find_value(request, "params");
    if (valParams.isArray())
        params = valParams.get_array();
    else if (valParams.isNull())
        params = UniValue(UniValue::VARR);
    else
        throw JSONRPCError(RPC_INVALID_REQUEST, "Params must be an array");
}


static UniValue JSONRPCExecOne(const UniValue& req)
{
    UniValue rpc_result(UniValue::VOBJ);

    JSONRequest jreq;
    try {
        jreq.parse(req);

        UniValue result = tableRPC.execute(jreq.strMethod, jreq.params);
        rpc_result = JSONRPCReplyObj(result, NullUniValue, jreq.id);
    } catch (UniValue& objError) {
        rpc_result = JSONRPCReplyObj(NullUniValue, objError, jreq.id);
    } catch (std::exception& e) {
        rpc_result = JSONRPCReplyObj(NullUniValue,
                                     JSONRPCError(RPC_PARSE_ERROR, e.what()), jreq.id);
    }

    return rpc_result;
}

static string JSONRPCExecBatch(const UniValue& vReq)
{
    UniValue ret(UniValue::VARR);
    for (unsigned int reqIdx = 0; reqIdx < vReq.size(); reqIdx++)
        ret.push_back(JSONRPCExecOne(vReq[reqIdx]));

    return ret.write() + "\n";
}

static bool HTTPReq_JSONRPC(AcceptedConnection* conn,
                            string& strRequest,
                            map<string, string>& mapHeaders,
                            bool fRun)
{
    //! Check authorization
    if (mapHeaders.count("authorization") == 0) {
        conn->stream() << HTTPError(HTTP_UNAUTHORIZED, false) << std::flush;
        return false;
    }

    if (!HTTPAuthorized(mapHeaders)) {
        LogPrintf("ThreadRPCServer incorrect password attempt from %s\n", conn->peer_address_to_string());
        /* Deter brute-forcing
           If this results in a DoS the user really
           shouldn't have their RPC port exposed. */
        MilliSleep(250);


        conn->stream() << HTTPError(HTTP_UNAUTHORIZED, false) << std::flush;
        return false;
    }

    JSONRequest jreq;
    try {
        //! Parse request
        UniValue valRequest;
        if (!valRequest.read(strRequest))
            throw JSONRPCError(RPC_PARSE_ERROR, "Parse error");

        //! Return immediately if in warmup
        {
            LOCK(cs_rpcWarmup);
            if (fRPCInWarmup)
                throw JSONRPCError(RPC_IN_WARMUP, rpcWarmupStatus);
        }

        string strReply;

        //! singleton request
        if (valRequest.isObject()) {
            jreq.parse(valRequest);

            UniValue result = tableRPC.execute(jreq.strMethod, jreq.params);

            //! Send reply
            strReply = JSONRPCReply(result, NullUniValue, jreq.id);

            //! array of requests
        } else if (valRequest.isArray())
            strReply = JSONRPCExecBatch(valRequest.get_array());
        else
            throw JSONRPCError(RPC_PARSE_ERROR, "Top-level object parse error");

        conn->stream() << HTTPReplyHeader(HTTP_OK, fRun, strReply.size()) << strReply << std::flush;
    } catch (UniValue& objError) {
        ErrorReply(conn->stream(), objError, jreq.id);
        return false;
    } catch (std::exception& e) {
        ErrorReply(conn->stream(), JSONRPCError(RPC_PARSE_ERROR, e.what()), jreq.id);
        return false;
    }
    return true;
}

void ServiceConnection(AcceptedConnection* conn)
{
    bool fRun = true;
    while (fRun) {
        int nProto = 0;
        map<string, string> mapHeaders;
        string strRequest, strMethod, strURI;

        //! Read HTTP request line
        if (!ReadHTTPRequestLine(conn->stream(), nProto, strMethod, strURI))
            break;

        //! Read HTTP message headers and body
        ReadHTTPMessage(conn->stream(), mapHeaders, strRequest, nProto, MAX_SIZE);

        //! HTTP Keep-Alive is false; close connection immediately
        if ((mapHeaders["connection"] == "close") || (!GetBoolArg("-rpckeepalive", true)))
            fRun = false;

        //! Process via JSON-RPC API
        if (strURI == "/") {
            if (!HTTPReq_JSONRPC(conn, strRequest, mapHeaders, fRun))
                break;

        //! Process via HTTP REST API
        } else if (strURI.substr(0, 6) == "/rest/" && GetBoolArg("-rest", false)) {
            if (!HTTPReq_REST(conn, strURI, strRequest, mapHeaders, fRun))
                break;

        } else {
            conn->stream() << HTTPError(HTTP_NOT_FOUND,false) << std::flush;
            break;
        }
    }
}

UniValue CRPCTable::execute(const std::string& strMethod, const UniValue& params) const
{
    //! Find method
    const CRPCCommand* pcmd = tableRPC[strMethod];
    if (!pcmd)
        throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Method not found");
#ifdef ENABLE_WALLET
    if (pcmd->reqWallet && !pwalletMain)
        throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Method not found (disabled)");
#endif

    //! Observe safe mode
    string strWarning = GetWarnings("rpc");
    if (strWarning != "" && !GetBoolArg("-disablesafemode", false) &&
        !pcmd->okSafeMode)
        throw JSONRPCError(RPC_FORBIDDEN_BY_SAFE_MODE, string("Safe mode: ") + strWarning);

    try {
        //! Execute
        UniValue result;
        {
            if (pcmd->threadSafe)
                result = pcmd->actor(params, false);
#ifdef ENABLE_WALLET
            else if (!pwalletMain) {
                LOCK(cs_main);
                result = pcmd->actor(params, false);
            } else {
                LOCK2(cs_main, pwalletMain->cs_wallet);
                result = pcmd->actor(params, false);
            }
#else  //! ENABLE_WALLET
            else {
                LOCK(cs_main);
                result = pcmd->actor(params, false);
            }
#endif //! ENABLE_WALLET
        }
        return result;
    } catch (std::exception& e) {
        throw JSONRPCError(RPC_MISC_ERROR, e.what());
    }
}

const CRPCTable tableRPC;
