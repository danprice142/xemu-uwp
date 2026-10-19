//
// xemu User Interface
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#pragma once
#include <memory>
#include "scene.hh"

class GameLibraryView;

// Full screen game library: box art grid over the selected title's artwork.
// Shown in place of the console at launch and from the popup menu.
class GameLibraryScene : public Scene
{
protected:
    std::unique_ptr<GameLibraryView> m_view;

public:
    GameLibraryScene();
    ~GameLibraryScene() override;
    void Show() override;
    void Hide() override;
    bool IsAnimating() override;
    bool Draw() override;
    bool IsActive();
};

extern GameLibraryScene g_game_library;
