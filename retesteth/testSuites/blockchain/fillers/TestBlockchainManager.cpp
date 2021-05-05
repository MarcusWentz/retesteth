#include "TestBlockchainManager.h"
#include <string>
using namespace std;

namespace test
{
namespace blockchainfiller
{
// Initialize blockchain manager with first chain information
// _env, _pre, _engine, _network
TestBlockchainManager::TestBlockchainManager(
    BlockchainTestFillerEnv const& _genesisEnv, State const& _genesisPre, SealEngine _engine, FORK const& _network)
  : m_session(RPCSession::instance(TestOutputHelper::getThreadID())),
    m_sDefaultChainName(BlockchainTestFillerBlock::defaultChainName()),
    m_genesisEnv(_genesisEnv),
    m_genesisPre(_genesisPre),
    m_sealEngine(_engine),
    m_sDefaultChainNet(_network)
{
    m_wasAtLeastOneFork = false;
    // m_sCurrentChainName is unknown at this point. the first block of the test defines it
    // but we want genesis to be generated anyway before that
    m_sCurrentChainName = m_sDefaultChainName;
    m_mapOfKnownChain.emplace(m_sCurrentChainName,
        TestBlockchain(_genesisEnv, _genesisPre, _engine, _network, m_sCurrentChainName, RegenerateGenesis::TRUE));
}

// Generate block using a client from the filler information
void TestBlockchainManager::parseBlockFromFiller(BlockchainTestFillerBlock const& _block, bool _generateUncles)
{
    ETH_LOGC("STARTING A NEW BLOCK: ", 6, LogColor::LIME);

    // See if chain reorg is needed. ex: new fork, or remine block
    reorgChains(_block);

    TestBlockchain& currentChainMining = getCurrentChain();

    // Prepare uncles using all chains and current debug info
    string sDebug = currentChainMining.prepareDebugInfoString(_block.chainName());

    // Check that we don't shift the chain after the initialization
    if (_block.hasChainNet())
    {
        ETH_ERROR_REQUIRE_MESSAGE(currentChainMining.getNetwork() == _block.chainNet(),
            "Trying to switch chainname with the following block! (chain: " + currentChainMining.getNetwork().asString() +
                ", block: " + _block.chainNet().asString() + ")");
    }

    // Generate UncleHeaders if we need it
    vectorOfSchemeBlock unclesPrepared = _generateUncles ? prepareUncles(_block, sDebug) : vectorOfSchemeBlock();

    // Generate the block
    currentChainMining.generateBlock(_block, unclesPrepared, _generateUncles);

    // Remeber the generated block in exact order as in the test
    TestBlock const& lastBlock = getLastBlock();

    // Get this block exception on canon chain to later verify it
    FORK const& canonNet = getDefaultChain().getNetwork();
    m_testBlockRLPs.push_back(std::make_tuple(lastBlock.getRawRLP(), _block.getExpectException(canonNet)));
}

TestBlockchain& TestBlockchainManager::getDefaultChain()
{
    assert(m_mapOfKnownChain.count(m_sDefaultChainName));
    return m_mapOfKnownChain.at(m_sDefaultChainName);
}

TestBlockchain& TestBlockchainManager::getCurrentChain()
{
    assert(m_mapOfKnownChain.count(m_sCurrentChainName));
    return m_mapOfKnownChain.at(m_sCurrentChainName);
}

TestBlock const& TestBlockchainManager::getLastBlock()
{
    assert(m_mapOfKnownChain.count(m_sCurrentChainName));
    TestBlockchain const& chain = m_mapOfKnownChain.at(m_sCurrentChainName);
    assert(chain.getBlocks().size() > 0);
    return chain.getBlocks().at(chain.getBlocks().size() - 1);
}

// Import all generated blocks at the same order as they are in tests
void TestBlockchainManager::syncOnRemoteClient(DataObject& _exportBlocksSection) const
{
    if (m_wasAtLeastOneFork)
    {
        // !!! RELY ON _exportBlocksSection has the same block order as m_testBlockRLPs
        ETH_LOGC("IMPORT KNOWN BLOCKS ", 6, LogColor::LIME);
        TestBlockchain const& chain = m_mapOfKnownChain.at(m_sDefaultChainName);
        chain.resetChainParams();  // restore canon chain of the test
        size_t ind = 0;
        for (auto const& rlpAndException : m_testBlockRLPs)
        {
            TestOutputHelper::get().setCurrentTestInfo(TestInfo(m_sDefaultChainNet.asString(), ind + 1, "AllKnown"));

            m_session.test_importRawBlock(std::get<0>(rlpAndException));
            string const& canonExcept = std::get<1>(rlpAndException);
            bool isValid = chain.checkBlockException(canonExcept);  // Check on canon exception
            if (!isValid)
            {
                DataObject& testObj = _exportBlocksSection.atUnsafe(ind);
                if (testObj.count("transactions"))
                    testObj.removeKey("transactions");
                if (testObj.count("uncleHeaders"))
                    testObj.removeKey("uncleHeaders");
                if (testObj.count("blockHeader"))
                    testObj.removeKey("blockHeader");
                testObj["expectException"] = canonExcept;
            }
            ind++;
        }
    }
}

vectorOfSchemeBlock TestBlockchainManager::prepareUncles(BlockchainTestFillerBlock const& _block, string const& _debug)
{
    ETH_LOGC("Prepare Uncles for the block: " + _debug, 6, LogColor::YELLOW);
    vectorOfSchemeBlock preparedUncleBlocks;  // Prepared uncles for the current block
    // return block header using uncle overwrite section on uncles array from test
    for (auto const& uncle : _block.uncles())
        preparedUncleBlocks.push_back(prepareUncle(uncle, preparedUncleBlocks));
    return preparedUncleBlocks;
}

void TestBlockchainManager::reorgChains(BlockchainTestFillerBlock const& _block)
{
    // if a new chain, initialize
    FORK const& newBlockChainNet = _block.hasChainNet() ? _block.chainNet() : m_sDefaultChainNet;
    VALUE const& newBlockNumber = _block.hasNumber() ? _block.number() : getCurrentChain().getBlocks().size();
    string const& newBlockChainName = _block.chainName();
    if (!m_mapOfKnownChain.count(newBlockChainName))
    {
        // Regenerate genesis only if the chain fork has changed
        m_mapOfKnownChain.emplace(newBlockChainName,
            TestBlockchain(m_genesisEnv, m_genesisPre, m_sealEngine, newBlockChainNet, newBlockChainName,
                m_sDefaultChainNet != newBlockChainNet ? RegenerateGenesis::TRUE : RegenerateGenesis::FALSE));
    }

    // Chain reorg conditions
    assert(m_mapOfKnownChain.count(newBlockChainName));
    const int blocksInChain = m_mapOfKnownChain.at(newBlockChainName).getBlocks().size() - 1;
    bool blockNumberHasDecreased = (newBlockNumber.asU256() != 0 && blocksInChain >= newBlockNumber.asU256());
    bool sameChain = (m_sCurrentChainName == newBlockChainName);

    if (!blockNumberHasDecreased && sameChain && newBlockNumber != 0)
    {
        // 0 is genesis. Check the block order:
        ETH_ERROR_REQUIRE_MESSAGE(newBlockNumber == blocksInChain + 1,
            "Require a `new blocknumber` == `previous blocknumber` + 1 has (" + newBlockNumber.asDecString() + " vs " +
                fto_string(blocksInChain + 1) + ")");

        VALUE actualNumberOnTheClient(m_session.eth_blockNumber() + 1);
        if (newBlockNumber != actualNumberOnTheClient)
            ETH_WARNING("Test mining blocknumber `" + newBlockNumber.asDecString() + "`, but client actually mine number: `" +
                        actualNumberOnTheClient.asDecString() + "`");
    }

    // if we switch the chain or have to remine one of blocknumbers
    if (!sameChain || blockNumberHasDecreased)
    {
        m_wasAtLeastOneFork = true;
        ETH_LOGC("PERFORM REWIND HISTORY:  (current: " + m_sCurrentChainName + ", new: " + newBlockChainName + ")", 6,
            LogColor::YELLOW);

        TestBlockchain& chain = m_mapOfKnownChain.at(newBlockChainName);
        if (!sameChain)
        {
            if (getCurrentChain().getNetwork() != chain.getNetwork())
                chain.resetChainParams();  // Reset genesis because chains have different config
            m_sCurrentChainName = newBlockChainName;
        }
        chain.restoreUpToNumber(m_session, newBlockNumber, sameChain && blockNumberHasDecreased);
    }
    m_session.test_modifyTimestamp(1000);  // Shift block timestamp relative to previous block
}

// Read test filler uncle section in block _uncleOverwrite
// And make uncle header out of it using information of currentChain and _currentBlockPreparedUncles
spBlockHeader TestBlockchainManager::prepareUncle(
    BlockchainTestFillerUncle _uncleSectionInTest, vectorOfSchemeBlock const& _currentBlockPreparedUncles)
{
    size_t origIndex = 0;
    BlockHeader const* tmpRefToSchemeBlock = NULL;
    TestBlockchain const& currentChainMining = getCurrentChain();

    UncleType typeOfSection = _uncleSectionInTest.uncleType();
    switch (typeOfSection)
    {
    case UncleType::SameAsPreviousBlockUncle:
    {
        // Make this uncle same as int(`sameAsPreviuousBlockUncle`) block's uncle
        size_t sameAsPreviuousBlockUncle = _uncleSectionInTest.sameAsPreviousBlockUncle();
        ETH_ERROR_REQUIRE_MESSAGE(currentChainMining.getBlocks().size() > sameAsPreviuousBlockUncle,
            "Trying to copy uncle from unregistered block with sameAsPreviuousBlockUncle!");
        ETH_ERROR_REQUIRE_MESSAGE(
            currentChainMining.getBlocks().at(sameAsPreviuousBlockUncle).getUncles().size() > 0,
            "Previous block has no uncles!");
        tmpRefToSchemeBlock = &currentChainMining.getBlocks().at(sameAsPreviuousBlockUncle).getUncles().at(0).getCContent();
        break;
    }
    case UncleType::PopulateFromBlock:
    {
        // _existingUncles (chain.getBlocks() ind 0 genesis, ind 1 - first block, ind 2 - second block)
        //  uncle populated from origIndex is stored at the next block as ForkBlock
        origIndex = _uncleSectionInTest.populateFromBlock();
        if (!_uncleSectionInTest.chainname().empty())
        {
            ETH_ERROR_REQUIRE_MESSAGE(
                m_mapOfKnownChain.count(_uncleSectionInTest.chainname()), "Uncle is populating from non-existent chain!");
            TestBlockchain const& chain = m_mapOfKnownChain.at(_uncleSectionInTest.chainname());
            ETH_ERROR_REQUIRE_MESSAGE(chain.getBlocks().size() > origIndex,
                "Trying to populate uncle from future block in another chain that has not been generated yet!");
            tmpRefToSchemeBlock = &chain.getBlocks().at(origIndex).getNextBlockForked().getCContent();
        }
        else
        {
            ETH_ERROR_REQUIRE_MESSAGE(currentChainMining.getBlocks().size() > origIndex,
                "Trying to populate uncle from future block that has not been generated yet!");
            tmpRefToSchemeBlock = &currentChainMining.getBlocks().at(origIndex).getNextBlockForked().getCContent();
        }
        break;
    }
    case UncleType::SameAsBlock:
    {
        EthGetBlockBy asBlock(m_session.eth_getBlockByNumber(_uncleSectionInTest.sameAsBlock(), Request::LESSOBJECTS));
        return asBlock.header();
    }
    case UncleType::SameAsPreviousSibling:
    {
        size_t siblingNumber = _uncleSectionInTest.sameAsPreviousSibling() - 1;  // 1 is first sib, 2 is next,...
        ETH_ERROR_REQUIRE_MESSAGE(siblingNumber < _currentBlockPreparedUncles.size(),
            "Trying to get uncle that has not been generated yet from current block!");
        tmpRefToSchemeBlock = &_currentBlockPreparedUncles.at(siblingNumber).getCContent();
        break;
    }
    default:
        ETH_ERROR_MESSAGE("Unhandled typeOfUncleSection!");
        break;
    }

    if (tmpRefToSchemeBlock == NULL)
        ETH_ERROR_MESSAGE("tmpRefToSchemeBlock is NULL!");
    spBlockHeader uncleBlockHeader = readBlockHeader((*tmpRefToSchemeBlock).asDataObject());

    // Perform uncle header modifications according to the uncle section in blockchain test filler block
    // If there is a field that is being overwritten in the uncle header
    if (_uncleSectionInTest.hasOverwriteHeader())
        uncleBlockHeader = _uncleSectionInTest.overwriteHeader().overwriteBlockHeader(uncleBlockHeader);

    // If uncle timestamp is shifted relative to the block that it's populated from
    if (typeOfSection == UncleType::PopulateFromBlock)
    {
        if (_uncleSectionInTest.hasRelTimestampFromPopulateBlock())
        {
            // Get the Timestamp of that block (which uncle is populated from)
            assert(currentChainMining.getBlocks().size() > origIndex);
            VALUE timestamp(currentChainMining.getBlocks().at(origIndex).getTestHeader().getCContent().timestamp());
            uncleBlockHeader.getContent().setTimestamp(
                timestamp.asU256() + _uncleSectionInTest.relTimestampFromPopulateBlock());
        }
    }

    // Recalculate uncleHash because we will be checking which uncle hash will be returned by the client
    uncleBlockHeader.getContent().recalculateHash();
    return uncleBlockHeader;
}

}  // namespace blockchainfiller
}  // namespace test
