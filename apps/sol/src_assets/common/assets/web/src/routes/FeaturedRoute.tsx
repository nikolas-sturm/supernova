/**
 * @file Featured apps route (legacy `featured.html`): app directory with
 * category filter and screenshot lightbox.
 */

import { useQuery } from '@tanstack/react-query'
import { formatDistanceToNow } from 'date-fns'
import {
  ArrowDownCircle,
  ChevronLeft,
  ChevronRight,
  CircleDot,
  ExternalLink,
  Gamepad,
  Gamepad2,
  GitFork,
  Globe,
  type LucideIcon,
  Monitor,
  Smartphone,
  Star,
  Tv,
} from 'lucide-react'
import { useEffect, useState } from 'react'
import { useTranslation } from 'react-i18next'
import type { FeaturedApp } from '../api/endpoints'
import { GitHubIcon } from '../components/BrandIcons'
import { Badge, Card, LinkButton, Spinner } from '../components/ui'
import { formatNumber } from '../lib/format'
import { featuredQuery } from '../queries'
import styles from './FeaturedRoute.module.css'
import { Page } from './shell'

/** Platform icon mapping. */
const platformIcons: Record<string, LucideIcon> = {
  android: Smartphone,
  console: Gamepad2,
  handheld: Gamepad,
  ios: Smartphone,
  linux: Monitor,
  macos: Monitor,
  tv: Tv,
  web: Globe,
  windows: Monitor,
}

/**
 * @brief Featured apps page component.
 * @returns The page element.
 */
export default function FeaturedRoute() {
  return (
    <Page>
      <FeaturedContent />
    </Page>
  )
}

/**
 * @brief Featured body once the shell is loaded.
 * @returns The page content.
 */
function FeaturedContent() {
  const { t } = useTranslation()
  const { data, isPending, isError } = useQuery(featuredQuery())
  const [selectedCategory, setSelectedCategory] = useState<string | null>(null)
  const [lightbox, setLightbox] = useState<{ screenshots: string[]; index: number } | null>(null)

  useEffect(() => {
    if (!lightbox) {
      return
    }
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'ArrowRight') {
        event.preventDefault()
        setLightbox((current) =>
          current ? { ...current, index: (current.index + 1) % current.screenshots.length } : null,
        )
      } else if (event.key === 'ArrowLeft') {
        event.preventDefault()
        setLightbox((current) =>
          current
            ? {
                ...current,
                index:
                  (current.index - 1 + current.screenshots.length) % current.screenshots.length,
              }
            : null,
        )
      } else if (event.key === 'Escape') {
        event.preventDefault()
        setLightbox(null)
      }
    }
    window.addEventListener('keydown', onKeyDown)
    return () => window.removeEventListener('keydown', onKeyDown)
  }, [lightbox])

  if (isPending) {
    return <Spinner />
  }

  if (isError || !data) {
    return (
      <Card>
        <h2>{t('_common.error')}</h2>
        <p>{t('featured.no_apps')}</p>
      </Card>
    )
  }

  const filtered = selectedCategory
    ? data.apps.filter((app) => app.category === selectedCategory)
    : data.apps
  const sorted = [...filtered].sort((a, b) => {
    if (a.official && !b.official) {
      return -1
    }
    if (!a.official && b.official) {
      return 1
    }
    return (b.github?.stars ?? 0) - (a.github?.stars ?? 0)
  })

  return (
    <>
      <div className="appsHeader">
        <h1>{t('featured.title')}</h1>
        <p>{t('featured.description')}</p>
      </div>

      <div className={styles.filterRow}>
        <button
          type="button"
          className={`${styles.filterButton} ${selectedCategory === null ? styles.filterActive : ''}`}
          onClick={() => setSelectedCategory(null)}
        >
          {t('_common.all')}
        </button>
        {data.categories.map((category) => (
          <button
            key={category.id}
            type="button"
            className={`${styles.filterButton} ${selectedCategory === category.id ? styles.filterActive : ''}`}
            onClick={() => setSelectedCategory(category.id)}
          >
            {t(`featured.categories.${category.originalId}`)}
          </button>
        ))}
      </div>

      {sorted.length === 0 ? (
        <p className="mutedCenter">{t('featured.no_apps')}</p>
      ) : (
        <div className={styles.grid}>
          {sorted.map((app) => (
            <FeaturedAppCard key={app.id} app={app} onOpenScreenshot={setLightbox} />
          ))}
        </div>
      )}

      {lightbox && lightbox.screenshots.length > 0 && (
        <div
          className={styles.lightbox}
          role="presentation"
          onClick={() => setLightbox(null)}
          onTouchStart={(event) => {
            const start = event.changedTouches[0]?.screenX ?? 0
            const target = event.currentTarget
            const onTouchEnd = (endEvent: TouchEvent) => {
              const end = endEvent.changedTouches[0]?.screenX ?? 0
              const diff = start - end
              if (Math.abs(diff) > 50) {
                setLightbox((current) =>
                  current
                    ? {
                        ...current,
                        index:
                          diff > 0
                            ? (current.index + 1) % current.screenshots.length
                            : (current.index - 1 + current.screenshots.length) %
                              current.screenshots.length,
                      }
                    : null,
                )
              }
              target.removeEventListener('touchend', onTouchEnd)
            }
            target.addEventListener('touchend', onTouchEnd)
          }}
        >
          <div className={styles.lightboxContent} onClick={(event) => event.stopPropagation()}>
            {lightbox.screenshots.length > 1 && (
              <>
                <button
                  type="button"
                  className={`${styles.lightboxNav} ${styles.lightboxPrev}`}
                  aria-label="Previous screenshot"
                  onClick={() =>
                    setLightbox((current) =>
                      current
                        ? {
                            ...current,
                            index:
                              (current.index - 1 + current.screenshots.length) %
                              current.screenshots.length,
                          }
                        : null,
                    )
                  }
                >
                  <ChevronLeft size={32} aria-hidden />
                </button>
                <button
                  type="button"
                  className={`${styles.lightboxNav} ${styles.lightboxNext}`}
                  aria-label="Next screenshot"
                  onClick={() =>
                    setLightbox((current) =>
                      current
                        ? { ...current, index: (current.index + 1) % current.screenshots.length }
                        : null,
                    )
                  }
                >
                  <ChevronRight size={32} aria-hidden />
                </button>
                <div className={styles.lightboxCounter}>
                  {lightbox.index + 1} / {lightbox.screenshots.length}
                </div>
              </>
            )}
            <img
              src={lightbox.screenshots[lightbox.index]}
              alt="Screenshot"
              onClick={(event) => event.stopPropagation()}
            />
          </div>
        </div>
      )}
    </>
  )
}

export interface FeaturedAppCardProps {
  /** The app entry. */
  app: FeaturedApp
  /** Opens the screenshot lightbox. */
  onOpenScreenshot: (state: { screenshots: string[]; index: number }) => void
}

/**
 * @brief One featured application card.
 * @param props Card props.
 * @returns The card element.
 */
function FeaturedAppCard({ app, onOpenScreenshot }: FeaturedAppCardProps) {
  const { t } = useTranslation()

  return (
    <Card className={styles.appCard}>
      <div className={styles.cardHead}>
        {app.icon ? (
          <img className={styles.icon} src={`${app.icon}?size=64`} alt={app.name} loading="lazy" />
        ) : (
          <div className={styles.iconPlaceholder}>
            <Globe size={28} aria-hidden />
          </div>
        )}
        <div className={styles.headText}>
          <h3>{app.name}</h3>
          {app.tagline && <p>{app.tagline}</p>}
        </div>
        {app.official && <Badge variant="primary">{t('featured.official')}</Badge>}
      </div>

      {app.description && <p className={styles.description}>{app.description}</p>}

      {app.github && (
        <div className={styles.stats}>
          {app.github.stars !== undefined && (
            <span title={t('featured.github_stars')}>
              <Star size={14} fill="currentColor" aria-hidden /> {formatNumber(app.github.stars)}
            </span>
          )}
          {app.github.forks !== undefined && (
            <span title={t('featured.github_forks')}>
              <GitFork size={14} aria-hidden /> {formatNumber(app.github.forks)}
            </span>
          )}
          {app.github.openIssues !== undefined && (
            <span title={t('featured.github_issues')}>
              <CircleDot size={14} aria-hidden /> {formatNumber(app.github.openIssues)}
            </span>
          )}
          {app.github.lastUpdated && (
            <span title={app.github.lastUpdated}>
              {t('featured.last_updated')}:{' '}
              {formatDistanceToNow(new Date(app.github.lastUpdated), { addSuffix: true })}
            </span>
          )}
        </div>
      )}

      {app.screenshots && app.screenshots.length > 0 && (
        <div className={styles.screenshots}>
          {app.screenshots.map((screenshot, index) => (
            <img
              key={screenshot}
              src={screenshot}
              alt={`${app.name} screenshot ${index + 1}`}
              loading="lazy"
              onClick={() => onOpenScreenshot({ screenshots: app.screenshots ?? [], index })}
            />
          ))}
        </div>
      )}

      {app.platforms && app.platforms.length > 0 && (
        <div className={styles.platforms}>
          {app.platforms.map((platform) => {
            const Icon = platformIcons[platform] ?? Globe
            return (
              <Badge key={platform} variant="neutral">
                <Icon size={14} aria-hidden />
                {platform}
              </Badge>
            )
          })}
        </div>
      )}

      <div className="buttonRow">
        {app.downloads && app.downloads.length > 0
          ? app.downloads.map((download) => (
              <LinkButton key={download.url} href={download.url} small>
                {download.img ? (
                  <img src={download.img} alt={download.label} height={40} />
                ) : (
                  <>
                    {download.url.startsWith('https://github.com') ? (
                      <GitHubIcon size={16} title="GitHub" />
                    ) : (
                      <ArrowDownCircle size={16} aria-hidden />
                    )}
                    {download.label}
                  </>
                )}
              </LinkButton>
            ))
          : app.links?.download && (
              <LinkButton href={app.links.download} small>
                <ArrowDownCircle size={16} aria-hidden />
                {t('featured.get')}
              </LinkButton>
            )}
        {app.links?.documentation && (
          <LinkButton
            href={app.links.documentation}
            small
            outline
            title={t('featured.documentation')}
          >
            <ExternalLink size={16} aria-hidden />
            {t('featured.docs')}
          </LinkButton>
        )}
        {app.links?.website && (
          <LinkButton href={app.links.website} small outline title={t('featured.website')}>
            <ExternalLink size={16} aria-hidden />
          </LinkButton>
        )}
        {app.links?.github && (
          <LinkButton href={app.links.github} small outline title={t('featured.github')}>
            <GitHubIcon size={16} title="GitHub" />
          </LinkButton>
        )}
      </div>
    </Card>
  )
}
