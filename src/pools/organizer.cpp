/**
 * Copyright (c) 2011-2015 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * libbitcoin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License with
 * additional permissions to the one published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version. For more information see LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/blockchain/pools/organizer.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <numeric>
#include <utility>
#include <thread>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/blockchain/interface/fast_chain.hpp>
#include <bitcoin/blockchain/pools/block_pool.hpp>
#include <bitcoin/blockchain/pools/organizer.hpp>
#include <bitcoin/blockchain/settings.hpp>
#include <bitcoin/blockchain/validation/fork.hpp>
#include <bitcoin/blockchain/validation/validate_block.hpp>

namespace libbitcoin {
namespace blockchain {

using namespace bc::chain;
using namespace bc::config;
using namespace std::placeholders;

#define NAME "organizer"

// Database access is limited to: push, pop, last-height, fork-difficulty,
// validator->populator:
// spend: { spender }
// block: { bits, version, timestamp }
// transaction: { exists, height, output }

static inline size_t cores(const settings& settings)
{
    const auto configured = settings.cores;
    const auto hardware = std::max(std::thread::hardware_concurrency(), 1u);
    return configured == 0 ? hardware : std::min(configured, hardware);
}

static inline thread_priority priority(const settings& settings)
{
    return settings.priority ? thread_priority::high : thread_priority::normal;
}

organizer::organizer(threadpool& thread_pool,
    fast_chain& chain, block_pool& block_pool, const settings& settings)
  : fast_chain_(chain),
    stopped_(true),
    flush_reorganizations_(settings.flush_reorganizations),
    block_pool_(block_pool),
    priority_pool_(cores(settings), priority(settings)),
    priority_dispatch_(priority_pool_, NAME "_priority"),
    validator_(priority_pool_, fast_chain_, settings),
    subscriber_(std::make_shared<reorganize_subscriber>(thread_pool, NAME)),
    dispatch_(thread_pool, NAME "_dispatch")
{
}

// Properties.
//-----------------------------------------------------------------------------

bool organizer::stopped() const
{
    return stopped_;
}

// Start/stop sequences.
//-----------------------------------------------------------------------------

bool organizer::start()
{
    stopped_ = false;
    subscriber_->start();

    // Don't begin flush lock if flushing on each reorganization.
    return flush_reorganizations_ || fast_chain_.begin_writes();
}

bool organizer::stop()
{
    validator_.stop();
    subscriber_->stop();
    subscriber_->invoke(error::service_stopped, 0, {}, {});

    // Ensure that this call blocks until database writes are complete.
    // Ensure no reorganization is in process when the flush lock is cleared.
    ///////////////////////////////////////////////////////////////////////////
    // Critical Section
    shared_lock lock(mutex_);

    // Ensure that a new validation will not begin after this stop. Otherwise
    // termination of the threadpool will corrupt the database.
    stopped_ = true;

    // Don't end flush lock if flushing on each reorganization.
    return flush_reorganizations_ || fast_chain_.end_writes();
    ///////////////////////////////////////////////////////////////////////////
}

// Organize sequence.
//-----------------------------------------------------------------------------

// This is called from block_chain::organize.
void organizer::organize(block_const_ptr block,
    result_handler handler)
{
    ///////////////////////////////////////////////////////////////////////////
    // Critical Section.
    // Use scope lock to guard the chain against concurrent organizations.
    // If a reorganization started after stop it will stop before writing.
    const auto lock = std::make_shared<scope_lock>(mutex_);

    if (stopped())
    {
        handler(error::service_stopped);
        return;
    }

    // TODO: defer deserialization using network stream.
    // Checks that are independent of chain state.
    const auto ec = validator_.check(block);

    if (ec)
    {
        handler(ec);
        return;
    }

    const result_handler locked_handler =
        std::bind(&organizer::complete,
            this, _1, lock, handler);

    // Get the path through the block forest to the new block.
    const auto fork = block_pool_.get_path(block);

    //*************************************************************************
    // CONSENSUS: This is the same check performed by satoshi, yet it will
    // produce a chain split in the case of a hash collision. This is because
    // it is not applied at the fork point, so some nodes will not see the
    // collision block and others will, depending on block order of arrival.
    // TODO: The hash check should start at the fork point. The duplicate check
    // is a conflated network denial of service protection mechanism and cannot
    // be allowed to reject blocks based on collisions not in the actual chain.
    // The block pool must be modified to accomodate hash collision as well.
    //*************************************************************************
    if (fork->empty() || fast_chain_.get_block_exists(block->hash()))
    {
        locked_handler(error::duplicate_block);
        return;
    }

    if (!set_fork_height(fork))
    {
        locked_handler(error::orphan_block);
        return;
    }

    // Verify the last fork block (all others are verified).
    // Preserve validation priority pool by returning on a network thread.
    const result_handler accept_handler =
        dispatch_.bound_delegate(&organizer::handle_accept,
            this, _1, fork, locked_handler);

    // Checks that are dependent on chain state and prevouts.
    // The fork may not have sufficient work to reorganize at this point, but
    // we must at least know if work required is sufficient in order to retain.
    validator_.accept(to_const(fork), accept_handler);
}

// private
void organizer::complete(const code& ec, scope_lock::ptr lock,
    result_handler handler)
{
    lock.reset();
    // End Critical Section.
    ///////////////////////////////////////////////////////////////////////////

    // This is the end of the organize sequence.
    handler(ec);
}

// Verify sub-sequence.
//-----------------------------------------------------------------------------

// private
void organizer::handle_accept(const code& ec, fork::ptr fork,
    result_handler handler)
{
    if (stopped())
    {
        handler(error::service_stopped);
        return;
    }

    if (ec)
    {
        handler(ec);
        return;
    }

    // Preserve validation priority pool by returning on a network thread.
    // This also protects our stack from exhaustion due to recursion.
    const result_handler connect_handler = 
        dispatch_.bound_delegate(&organizer::handle_connect,
            this, _1, fork, handler);

    // Checks that include script validation.
    validator_.connect(to_const(fork), connect_handler);
}

// private
void organizer::handle_connect(const code& ec, fork::ptr fork,
    result_handler handler)
{
    if (stopped())
    {
        handler(error::service_stopped);
        return;
    }

    if (ec)
    {
        handler(ec);
        return;
    }

    const auto first_height = fork->height() + 1u;
    const auto maximum = fork->difficulty();
    uint256_t threshold;

    // The chain query will stop if it reaches the maximum.
    if (!fast_chain_.get_fork_difficulty(threshold, maximum, first_height))
    {
        handler(error::operation_failed);
        return;
    }

    if (fork->difficulty() <= threshold)
    {
        block_pool_.add(fork->top());
        handler(error::insufficient_work);
        return;
    }

    // The top block is valid.
    const auto top = fork->top();
    top->header().validation.height = fork->top_height();
    top->validation.error = error::success;
    top->validation.start_notify = asio::steady_clock::now();

    // Get the outgoing blocks to forward to reorg handler.
    const auto out_blocks = std::make_shared<block_const_ptr_list>();
    
    const auto complete =
        std::bind(&organizer::handle_reorganized,
            this, _1, to_const(fork), out_blocks, handler);

    // Replace! Switch!
    //#########################################################################
    fast_chain_.reorganize(to_const(fork), out_blocks, flush_reorganizations_,
        priority_dispatch_, complete);
    //#########################################################################
}

// private
void organizer::handle_reorganized(const code& ec, fork::const_ptr fork,
    block_const_ptr_list_ptr outgoing, result_handler handler)
{
    if (ec)
    {
        LOG_FATAL(LOG_BLOCKCHAIN)
            << "Failure writing block to store, is now corrupted: "
            << ec.message();
        handler(ec);
        return;
    }

    block_pool_.remove(fork->blocks());
    block_pool_.prune(fork->top_height());
    block_pool_.add(outgoing);

    // TODO: we can notify before reorg for mining scenario.
    // v3 reorg block order is reverse of v2, fork.back() is the new top.
    notify_reorganize(fork->height(), fork->blocks(), to_const(outgoing));

    // This is the end of the verify sub-sequence.
    handler(error::success);
}

// Subscription.
//-----------------------------------------------------------------------------

void organizer::subscribe_reorganize(reorganize_handler&& handler)
{
    subscriber_->subscribe(std::move(handler),
        error::service_stopped, 0, {}, {});
}

// private
void organizer::notify_reorganize(size_t fork_height,
    block_const_ptr_list_const_ptr fork,
    block_const_ptr_list_const_ptr original)
{
    // Invoke is required here to prevent subscription parsing from creating a
    // unsurmountable backlog during catch-up sync.
    subscriber_->invoke(error::success, fork_height, fork, original);
}

// Utility.
//-----------------------------------------------------------------------------

// private
bool organizer::set_fork_height(fork::ptr fork)
{
    BITCOIN_ASSERT(!fork->empty());

    size_t height;

    // Get blockchain parent of the oldest fork block (orphan if false).
    if (!fast_chain_.get_height(height, fork->hash()))
        return false;

    // Guard against chain size overflow.
    safe_add(height, fork->size());

    fork->set_height(height);
    return true;
}

} // namespace blockchain
} // namespace libbitcoin
