/**
 * @file Resource links card (website, docs, Discord, GitHub, legal).
 * Ported from the legacy `ResourceCard.vue`.
 */

import { AlertCircle, BookOpen, FileText, Globe } from 'lucide-react'
import { useTranslation } from 'react-i18next'
import { DiscordIcon, GitHubIcon } from '../components/BrandIcons'
import { Card, LinkButton } from '../components/ui'

export interface ResourceCardProps {
  /** Whether the installed version is newer than the latest stable release. */
  installedVersionNotStable?: boolean
}

/**
 * @brief Card with project resource links and legal links.
 * @param props Card props.
 * @returns The card elements.
 */
export function ResourceCard({ installedVersionNotStable = false }: ResourceCardProps) {
  const { t } = useTranslation()
  const docsVersion = installedVersionNotStable ? 'master' : 'latest'
  const documentationUrl = `https://docs.lizardbyte.dev/projects/sunshine/${docsVersion}/`

  return (
    <>
      <Card title={t('resource_card.resources')}>
        <div className="buttonRow">
          <LinkButton href="https://app.lizardbyte.dev" variant="success">
            <Globe size={18} aria-hidden />
            {t('resource_card.lizardbyte_website')}
          </LinkButton>
          <LinkButton href={documentationUrl} variant="info">
            <BookOpen size={18} aria-hidden />
            {t('resource_card.documentation')}
          </LinkButton>
          <LinkButton href="https://app.lizardbyte.dev/discord">
            <DiscordIcon size={18} title="Discord" />
            Discord
          </LinkButton>
          <LinkButton href="https://github.com/orgs/LizardByte/discussions" variant="secondary">
            <GitHubIcon size={18} title="GitHub" />
            {t('resource_card.github_discussions')}
          </LinkButton>
        </div>
      </Card>
      <Card title={t('resource_card.legal')}>
        <p>{t('resource_card.legal_desc')}</p>
        <div className="buttonRow">
          <LinkButton
            href="https://github.com/LizardByte/Sunshine/blob/master/LICENSE"
            variant="danger"
          >
            <FileText size={18} aria-hidden />
            {t('resource_card.license')}
          </LinkButton>
          <LinkButton
            href="https://github.com/LizardByte/Sunshine/blob/master/NOTICE"
            variant="danger"
          >
            <AlertCircle size={18} aria-hidden />
            {t('resource_card.third_party_notice')}
          </LinkButton>
        </div>
      </Card>
    </>
  )
}
