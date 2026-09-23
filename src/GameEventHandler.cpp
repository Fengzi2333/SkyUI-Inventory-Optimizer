#include "GameEventHandler.h"
#include "Hooks.h"
#include "ProfilingHooks.h"
#if BOOST_FOUND
#include <boost/algorithm/algorithm.hpp>
#endif

namespace plugin {
    void GameEventHandler::onLoad() {
        logger::info("onLoad()");
#if BOOST_FOUND
            logger::info("boost found -- {}", boost::algorithm::power(2, 4));
#endif
        Hooks::install();
    }

    void GameEventHandler::onPostLoad() {
        logger::info("onPostLoad()");
    }

    void GameEventHandler::onPostPostLoad() {
        logger::info("onPostPostLoad()");
    }

    void GameEventHandler::onInputLoaded() {
        logger::info("onInputLoaded()");
    }

    void GameEventHandler::onDataLoaded() {
        logger::info("onDataLoaded()");
        ProfilingHooks::installTimelineMarkers();
    }

    void GameEventHandler::onNewGame() {
        logger::info("onNewGame()");
    }

    void GameEventHandler::onPreLoadGame() {
        logger::info("onPreLoadGame()");
    }

    void GameEventHandler::onPostLoadGame() {
        logger::info("onPostLoadGame()");
        // RE::UI is guaranteed to exist once a game is loaded; retries the
        // registration in case kDataLoaded was too early.
        ProfilingHooks::installTimelineMarkers();
    }

    void GameEventHandler::onSaveGame() {
        logger::info("onSaveGame()");
    }

    void GameEventHandler::onDeleteGame() {
        logger::info("onDeleteGame()");
    }
}  // namespace plugin