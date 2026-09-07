/**
 * @file Rerenders the active config tab body.
 */

import { AdvancedTab } from './tabs/AdvancedTab'
import { AudioVideoTab } from './tabs/AudioVideoTab'
import { EncoderTabs } from './tabs/EncoderTabs'
import { FilesTab } from './tabs/FilesTab'
import { GeneralTab } from './tabs/GeneralTab'
import { InputTab } from './tabs/InputTab'
import { NetworkTab } from './tabs/NetworkTab'

/** Props shared by every tab body component. */
export interface TabBodyProps {
  /** The editable config draft. */
  draft: Record<string, unknown>
  /**
   * @brief Writes one value back to the draft.
   * @param key The config key.
   * @param value The new value.
   */
  setDraftValue: (key: string, value: unknown) => void
  /** Host platform. */
  platform: string
}

export interface TabProps extends TabBodyProps {
  /** Active tab id. */
  tabId: string
}

/**
 * @brief Selects and renders the active tab component.
 * @param props Tab context props.
 * @returns The tab content.
 */
export function TabContent({ tabId, draft, setDraftValue, platform }: TabProps) {
  switch (tabId) {
    case 'general':
      return <GeneralTab draft={draft} setDraftValue={setDraftValue} platform={platform} />
    case 'input':
      return <InputTab draft={draft} setDraftValue={setDraftValue} platform={platform} />
    case 'av':
      return <AudioVideoTab draft={draft} setDraftValue={setDraftValue} platform={platform} />
    case 'network':
      return <NetworkTab draft={draft} setDraftValue={setDraftValue} platform={platform} />
    case 'files':
      return <FilesTab draft={draft} setDraftValue={setDraftValue} platform={platform} />
    case 'advanced':
      return <AdvancedTab draft={draft} setDraftValue={setDraftValue} platform={platform} />
    default:
      return (
        <EncoderTabs
          tabId={tabId}
          draft={draft}
          setDraftValue={setDraftValue}
          platform={platform}
        />
      )
  }
}
