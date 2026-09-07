/**
 * @file Safe markdown renderer for release notes.
 *
 * Unlike the legacy `marked` + `v-html` pipeline (which rendered raw HTML),
 * react-markdown escapes embedded HTML by default, closing the XSS vector.
 */

import ReactMarkdown from 'react-markdown'

export interface MarkdownProps {
  /** Markdown source text. */
  children: string
}

/**
 * @brief Renders markdown text to sanitized HTML elements.
 * @param props Markdown source.
 * @returns The rendered markdown.
 */
export function Markdown({ children }: MarkdownProps) {
  return (
    <div className="markdown-body">
      <ReactMarkdown
        components={{
          a: (props) => <a {...props} target="_blank" rel="noopener noreferrer" />,
        }}
      >
        {children}
      </ReactMarkdown>
    </div>
  )
}
