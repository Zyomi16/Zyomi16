"""Update dark_mode.svg / light_mode.svg with live GitHub stats.
Env: ACCESS_TOKEN (GitHub token), USER_NAME (GitHub login)."""
import os, re, datetime, requests

TOKEN = os.environ.get('ACCESS_TOKEN', '')
USER = os.environ.get('USER_NAME', '')
API = 'https://api.github.com/graphql'
SVGS = ['dark_mode.svg', 'light_mode.svg']


def gql(query, variables):
    r = requests.post(API, json={'query': query, 'variables': variables},
                      headers={'Authorization': f'bearer {TOKEN}'}, timeout=60)
    r.raise_for_status()
    data = r.json()
    if 'errors' in data:
        raise RuntimeError(data['errors'])
    return data['data']


def repo_and_star_stats():
    q = '''query($login:String!,$cursor:String){user(login:$login){
      createdAt followers{totalCount}
      repositories(first:100,after:$cursor,ownerAffiliations:OWNER){
        totalCount nodes{stargazerCount} pageInfo{hasNextPage endCursor}}}}'''
    cursor, stars = None, 0
    while True:
        u = gql(q, {'login': USER, 'cursor': cursor})['user']
        repos = u['repositories']
        stars += sum(n['stargazerCount'] for n in repos['nodes'])
        if not repos['pageInfo']['hasNextPage']:
            return repos['totalCount'], stars, u['followers']['totalCount'], u['createdAt']
        cursor = repos['pageInfo']['endCursor']


def total_contributions(created_at):
    q = '''query($login:String!,$from:DateTime!,$to:DateTime!){user(login:$login){
      contributionsCollection(from:$from,to:$to){contributionCalendar{totalContributions}}}}'''
    total = 0
    for year in range(int(created_at[:4]), datetime.date.today().year + 1):
        d = gql(q, {'login': USER, 'from': f'{year}-01-01T00:00:00Z', 'to': f'{year}-12-31T23:59:59Z'})
        total += d['user']['contributionsCollection']['contributionCalendar']['totalContributions']
    return total


def set_value(svg, elem_id, value):
    value = f'{value:,}' if isinstance(value, int) else str(value)
    m = re.search(rf'id="{elem_id}" data-len="(\d+)"', svg)
    avail = int(m.group(1))
    dots = ' ' + '.' * max(1, avail - len(value) - 2) + ' '
    svg = re.sub(rf'(id="{elem_id}_dots">)[^<]*(<)', lambda x: x.group(1) + dots + x.group(2), svg)
    return re.sub(rf'(id="{elem_id}" data-len="\d+">)[^<]*(<)', lambda x: x.group(1) + value + x.group(2), svg)


def update_svgs(stats):
    for path in SVGS:
        with open(path, encoding='utf-8') as f:
            svg = f.read()
        for key, val in stats.items():
            svg = set_value(svg, key, val)
        with open(path, 'w', encoding='utf-8') as f:
            f.write(svg)


if __name__ == '__main__':
    repos, stars, followers, created = repo_and_star_stats()
    update_svgs({'repo_data': repos, 'star_data': stars,
                 'follower_data': followers, 'commit_data': total_contributions(created)})
    print('updated:', repos, stars, followers)
