/*
 * Strawberry Music Player
 * This file was part of Clementine.
 * Copyright 2010, David Sansome <me@davidsansome.com>
 * Copyright 2018-2026, Jonas Kvinge <jonas@jkvinge.net>
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <random>

#include <QIODevice>
#include <QDataStream>
#include <QByteArray>
#include <QString>
#include <QVariantList>
#include <QThread>
#include <QMutex>
#include <QMutexLocker>
#include <QCoreApplication>

#include "playlist/playlist.h"
#include "playlistquerygenerator.h"
#include "collection/collectionbackend.h"

PlaylistQueryGenerator::PlaylistQueryGenerator(QObject *parent) : PlaylistGenerator(parent), dynamic_(false), current_pos_(0) {}

PlaylistQueryGenerator::PlaylistQueryGenerator(const QString &name, const SmartPlaylistSearch &search, const bool dynamic, QObject *parent)
    : PlaylistGenerator(parent),
      search_(search),
      dynamic_(dynamic),
      current_pos_(0) {

  set_name(name);

}

void PlaylistQueryGenerator::Load(const SmartPlaylistSearch &search) {

  QMutexLocker l(&mutex_);
  search_ = search;
  dynamic_ = false;
  current_pos_ = 0;

}

void PlaylistQueryGenerator::Load(const QByteArray &data) {

  QMutexLocker l(&mutex_);
  QDataStream s(data);
  s >> search_;
  s >> dynamic_;

}

QByteArray PlaylistQueryGenerator::Save() const {

  QByteArray ret;
  QDataStream s(&ret, QIODevice::WriteOnly);
  s << search_;
  s << dynamic_;

  return ret;

}

PlaylistItemPtrList PlaylistQueryGenerator::Generate() {

  QMutexLocker l(&mutex_);
  previous_ids_.clear();
  current_pos_ = 0;
  return GenerateMoreLocked(0);

}

PlaylistItemPtrList PlaylistQueryGenerator::FinalizeGenerate(const SongList& songs) {

  PlaylistItemPtrList items;

  items.reserve(songs.count());
  for (const Song &song : songs) {
    // In sorted order the offset can return a song that was already added, when songs earlier in the order were added to the collection after the previous call, so skip those.
    if (previous_ids_.contains(song.id())) continue;
    items << PlaylistItem::NewFromSong(song);
    previous_ids_ << song.id();
  }
  return items;

}

PlaylistItemPtrList PlaylistQueryGenerator::GenerateMore(const int count) {

  QMutexLocker l(&mutex_);
  return GenerateMoreLocked(count);

}

PlaylistItemPtrList PlaylistQueryGenerator::GenerateMoreLocked(const int count) {

  SmartPlaylistSearch search_copy = search_;
  search_copy.unlimited_ = (grouped_mode_ != 1);
  if (count > 0) {
    search_copy.limit_ = count;
  }

  // In random order exclude the songs already added, in sorted order continue after them instead.
  // Doing both would skip as many songs as were already added, since the offset counts the rows after excluding them.
  const bool random = search_copy.sort_type_ == SmartPlaylistSearch::SortType::Random;
  if (random) {
    search_copy.id_not_in_ = previous_ids_;
  }
  else {
    search_copy.first_item_ = current_pos_;
    if (search_copy.unlimited_) {
      search_copy.limit_ = -1;
    }
  }

  QVariantList bound_values;
  const QString sql = search_copy.ToSql(collection_backend_->songs_table(), bound_values);
  const SongList songs = collection_backend_->ExecuteQuery(sql, bound_values);

  // Continue after the songs that were returned, the limit can be -1 for no limit.
  if (!random) {
    current_pos_ += static_cast<int>(songs.count());
  }

  // When called in a concurrent thread, close the unique DB connection.
  if (QThread::currentThread() != collection_backend_->thread() && QThread::currentThread() != QCoreApplication::instance()->thread()) {
    collection_backend_->Close();
  }

  if (search_copy.unlimited_) {
    Playlist random_playlist(nullptr, nullptr, nullptr, nullptr, nullptr, 0);
    SongList local_song;
    const int local_limit = count > 0 ? count : search_.limit_;

    random_playlist.InsertSongs(songs);
    random_playlist.Shuffle(PlaylistSequence::ShuffleMode::Grouping);

    if (grouped_mode_ == 0 || local_limit >= songs.count()) {
      for (PlaylistItemPtr& current_item : random_playlist.items()) {
        if (local_song.count() >= local_limit && local_limit > 0 && !local_song.last().IsOnSameGrouping(current_item->EffectiveMetadata())) {
          break;
        }
        local_song << current_item->EffectiveMetadata();
      }
    }
    else {
      static std::mt19937 rng{std::random_device{}()};
      std::uniform_int_distribution<int> rand_number(0, songs.count() - local_limit);
      int left_grouped = grouped_mode_;
      int left_continue = rand_number(rng);

      for (PlaylistItemPtr& current_item : random_playlist.items()) {
        if (left_continue > 0) {
          --left_continue;
          continue;
        }
        if (local_song.count() > 0 && local_song.last().IsOnSameGrouping(current_item->EffectiveMetadata())) {
          --left_grouped;
        }
        else {
          left_grouped = grouped_mode_;
        }
        if (local_song.count() >= local_limit && local_limit > 0 && left_grouped <= 0) {
          break;
        }
        local_song << current_item->EffectiveMetadata();
      }
    }

    return FinalizeGenerate(local_song);
  }

  return FinalizeGenerate(songs);

}
