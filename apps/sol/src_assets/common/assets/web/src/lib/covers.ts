/**
 * @file Cover art search against the LizardByte GameDB static database.
 * Ported from the legacy apps page.
 */

/** One cover art candidate returned by the search. */
export interface CoverCandidate {
  /** Game name from GameDB. */
  name: string
  /** Stable key identifying the game (used for the upload request). */
  key: string
  /** Thumbnail URL to preview. */
  url: string
  /** Full-resolution URL persisted by the backend. */
  saveUrl: string
}

const GAMEDB_BASE_URL = 'https://raw.githubusercontent.com/LizardByte/GameDB/gh-pages'

/**
 * @brief Computes the GameDB bucket key for a search term.
 * @param name The game name.
 * @returns Two-letter bucket, or "@" when the name has no usable prefix.
 */
export function getSearchBucket(name: string): string {
  const bucket = name
    .substring(0, Math.min(name.length, 2))
    .toLowerCase()
    .replaceAll(/[^a-z\d]/g, '')
  return bucket || '@'
}

/**
 * @brief Searches GameDB for cover art matching the given name.
 * @param name The game name to search for.
 * @returns Matching cover candidates; empty when nothing matches.
 */
export async function searchCovers(name: string): Promise<CoverCandidate[]> {
  if (!name) {
    return []
  }
  const searchName = name.replaceAll(/\s+/g, '.').toLowerCase()
  const bucket = getSearchBucket(name)

  const bucketResponse = await fetch(`${GAMEDB_BASE_URL}/buckets/${bucket}.json`)
  if (!bucketResponse.ok) {
    throw new Error('Failed to search covers')
  }
  const maps: Record<string, { name: string }> = await bucketResponse.json()

  const gameIds = Object.keys(maps).filter((id) => {
    const item = maps[id]
    return item ? item.name.replaceAll(/\s+/g, '.').toLowerCase().startsWith(searchName) : false
  })

  const games = await Promise.all(
    gameIds.map(async (id) => {
      try {
        const response = await fetch(`${GAMEDB_BASE_URL}/games/${id}.json`)
        return response.ok ? ((await response.json()) as GameDbGame) : null
      } catch {
        return null
      }
    }),
  )

  const gamesWithCovers = games.filter(
    (game): game is GameDbGame & { cover: { url: string } } =>
      game !== null && game.cover !== undefined && game.cover.url !== '',
  )

  return gamesWithCovers
    .map((game) => {
      const thumb = game.cover.url
      const dotIndex = thumb.lastIndexOf('.')
      const slashIndex = thumb.lastIndexOf('/')
      if (dotIndex < 0 || slashIndex < 0) {
        return null
      }
      const slug = thumb.substring(slashIndex + 1, dotIndex)
      return {
        name: game.name,
        key: `igdb_${game.id}`,
        url: `https://images.igdb.com/igdb/image/upload/t_cover_big/${slug}.jpg`,
        saveUrl: `https://images.igdb.com/igdb/image/upload/t_cover_big_2x/${slug}.png`,
      }
    })
    .filter((item): item is CoverCandidate => item !== null)
}

interface GameDbGame {
  id: number
  name: string
  cover?: {
    url: string
  }
}
