// Node 22+ supplies the original numeric token, so editing a history never
// rounds signed 64-bit scores or unrelated integer metadata.
export function parseExact(text: string): any {
  return JSON.parse(text, ((_: string, value: unknown, context?: {source?: string}) => {
    return typeof value === 'number' && context?.source && /^-?\d+$/.test(context.source)
      ? BigInt(context.source) : value;
  }));
}

export function stringifyExact(value: unknown, pretty = false): string {
  const json = JSON as typeof JSON & {rawJSON(value: string): unknown};
  return JSON.stringify(value, (_, item: unknown) => typeof item === 'bigint'
    ? json.rawJSON(item.toString()) : item, pretty ? 2 : undefined);
}
