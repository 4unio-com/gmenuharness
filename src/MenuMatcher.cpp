/*
 * Copyright © 2014 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Pete Woods <pete.woods@canonical.com>
 */

#include <unity/gmenuharness/MenuMatcher.h>
#include <unity/gmenuharness/MatchUtils.h>

#include <chrono>
#include <iostream>

#include <gio/gio.h>

using namespace std;

namespace unity
{

namespace gmenuharness
{

namespace
{

static void gdbus_connection_deleter(GDBusConnection* connection)
{
    g_clear_object(&connection);
}
}

struct MenuMatcher::Parameters::Priv
{
    string m_busName;

    vector<pair<string, string>> m_actions;

    string m_menuObjectPath;
};

MenuMatcher::Parameters::Parameters(const string& busName,
                                    const vector<pair<string, string>>& actions,
                                    const string& menuObjectPath) :
        p(new Priv)
{
    p->m_busName = busName;
    p->m_actions = actions;
    p->m_menuObjectPath = menuObjectPath;
}

MenuMatcher::Parameters::~Parameters()
{
}

MenuMatcher::Parameters::Parameters(const Parameters& other) :
        p(new Priv)
{
    *this = other;
}

MenuMatcher::Parameters::Parameters(Parameters&& other)
{
    *this = move(other);
}

MenuMatcher::Parameters& MenuMatcher::Parameters::operator=(const Parameters& other)
{
    p->m_busName = other.p->m_busName;
    p->m_actions = other.p->m_actions;
    p->m_menuObjectPath = other.p->m_menuObjectPath;
    return *this;
}

MenuMatcher::Parameters& MenuMatcher::Parameters::operator=(Parameters&& other)
{
    p = move(other.p);
    return *this;
}

struct MenuMatcher::Priv
{
    Priv(const Parameters& parameters) :
        m_parameters(parameters)
    {
    }

    Parameters m_parameters;

    vector<MenuItemMatcher> m_items;

    shared_ptr<GDBusConnection> m_system;

    shared_ptr<GDBusConnection> m_session;

    shared_ptr<GMenuModel>  m_menu;

    map<string, shared_ptr<GActionGroup>> m_actions;

    shared_ptr<GDBusConnection> createDBusConnection(GBusType type)
	{
		GError *error = nullptr;

		gchar *address = g_dbus_address_get_for_bus_sync(type,
				nullptr, &error);
		if (!address)
		{
			g_assert(error != nullptr);
			if (error->domain != G_IO_ERROR
					|| error->code != G_IO_ERROR_CANCELLED)
			{
				std::cerr << "Error getting the bus address: "
						<< error->message;
			}
			g_error_free(error);

			throw runtime_error("Unable to get DBus connection address");
		}

		error = nullptr;

		shared_ptr<GDBusConnection> bus(
				g_dbus_connection_new_for_address_sync(address,
						(GDBusConnectionFlags) (G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT
								| G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
						nullptr, nullptr, &error), &gdbus_connection_deleter);
		g_free(address);

		if (!bus)
		{
			g_assert(error != nullptr);
			if (error->domain != G_IO_ERROR
					|| error->code != G_IO_ERROR_CANCELLED)
			{
				std::cerr << "Error getting the bus: " << error->message;
			}
			g_error_free(error);
			throw runtime_error("Unable to connect to DBus");
		}

		g_dbus_connection_set_exit_on_close(bus.get(), FALSE);

		return bus;
	}

    void createGmenu()
    {
        m_system = createDBusConnection(G_BUS_TYPE_SYSTEM);
        m_session = createDBusConnection(G_BUS_TYPE_SESSION);

        m_menu.reset(
                G_MENU_MODEL(
                        g_dbus_menu_model_get(
                                m_session.get(),
                                m_parameters.p->m_busName.c_str(),
                                m_parameters.p->m_menuObjectPath.c_str())),
                &g_object_deleter);

        for (const auto& action : m_parameters.p->m_actions)
        {
            shared_ptr<GActionGroup> actionGroup(
                    G_ACTION_GROUP(
                            g_dbus_action_group_get(
                                    m_session.get(),
                                    m_parameters.p->m_busName.c_str(),
                                    action.second.c_str())),
                    &g_object_deleter);
            m_actions[action.first] = actionGroup;
        }
    }
};

MenuMatcher::MenuMatcher(const Parameters& parameters) :
        p(new Priv(parameters))
{
    p->createGmenu();
}

MenuMatcher::~MenuMatcher()
{
}

MenuMatcher& MenuMatcher::item(const MenuItemMatcher& item)
{
    p->m_items.emplace_back(item);
    return *this;
}

static chrono::time_point<chrono::system_clock> topLevelTimeout() {
    return chrono::system_clock::now() + chrono::seconds(20);
}

void MenuMatcher::match(MatchResult& matchResult) const
{
    vector<unsigned int> location;

    auto timeout = topLevelTimeout();

    while (true)
    {
        MatchResult childMatchResult(matchResult.createChild());

        int menuSize = g_menu_model_get_n_items(p->m_menu.get());
        if (p->m_items.size() > (unsigned int) menuSize)
        {
            childMatchResult.failure(
                    location,
                    "Row count mismatch, expected " + to_string(p->m_items.size())
                            + " but found " + to_string(menuSize));
        }
        else
        {
            for (size_t i = 0; i < p->m_items.size(); ++i)
            {
                const auto& matcher = p->m_items.at(i);
                matcher.match(childMatchResult, location, p->m_menu, p->m_actions, i);
            }
        }

        if (childMatchResult.success())
        {
            matchResult.merge(childMatchResult);
            break;
        }
        else
        {
            if (chrono::system_clock::now() >= timeout)
            {
                // Start with a fresh menu to work around initialisation race condition in GMenu
                p->createGmenu();

                timeout = topLevelTimeout();
            }

            if (matchResult.hasTimedOut())
            {
                matchResult.merge(childMatchResult);
                break;
            }
            menuWaitForItems(p->m_menu);
        }
    }
}

MatchResult MenuMatcher::match() const
{
    MatchResult matchResult;
    match(matchResult);
    return matchResult;
}

}   // namespace gmenuharness

}   // namespace unity
