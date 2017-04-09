/***************************************************************************
 *   Copyright (C) 2017 by Nicolas Carion                                  *
 *   This file is part of Kdenlive. See www.kdenlive.org.                  *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) version 3 or any later version accepted by the       *
 *   membership of KDE e.V. (or its successor approved  by the membership  *
 *   of KDE e.V.), which shall act as a proxy defined in Section 14 of     *
 *   version 3 of the license.                                             *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

template<typename Service>
MoveableItem<Service>::MoveableItem(std::weak_ptr<TimelineModel> parent, int id) :
    m_parent(parent)
    , m_id(id == -1 ? TimelineModel::getNextId() : id)
    , m_position(-1)
    , m_currentTrackId(-1)
{
}


template<typename Service>
int MoveableItem<Service>::getId() const
{
    return m_id;
}

template<typename Service>
int MoveableItem<Service>::getCurrentTrackId() const
{
    return m_currentTrackId;
}

template<typename Service>
int MoveableItem<Service>::getPosition() const
{
    return m_position;
}

template<typename Service>
std::pair<int, int> MoveableItem<Service>::getInOut() const
{
    return {getIn(), getOut()};
}

template<typename Service>
int MoveableItem<Service>::getIn() const
{
    return service()->get_in();
}

template<typename Service>
int MoveableItem<Service>::getOut() const
{
    return service()->get_out();
}

template<typename Service>
bool MoveableItem<Service>::isValid()
{
    return service()->is_valid();
}


template<typename Service>
void MoveableItem<Service>::setPosition(int pos)
{
    m_position = pos;
}

template<typename Service>
void MoveableItem<Service>::setCurrentTrackId(int tid)
{
    m_currentTrackId = tid;
}

template<typename Service>
void MoveableItem<Service>::setInOut(int in, int out)
{
    m_position = in;
    service()->set_in_and_out(in, out);
}

