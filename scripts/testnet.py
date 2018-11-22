import json
import subprocess

params = {
    'cleos_path': "cleos",
    'testnet_url': "http://cyberway-mongodb.golos.io:8888",
}
cleosCmd = "{cleos_path} --url {testnet_url} ".format(**params)

class args:
    symbol = "GLS"
    token_precision = 3
    vesting_precision = 6
    token = '%d,%s' % (token_precision, symbol)
    vesting = '%d,%s' % (vesting_precision, symbol)
    root_private = '5K463ynhZoCDDa4RDcr63cUwWLTnKqmdcoTKTHBjqoKfv4u5V7p'
    root_public = 'GLS8Znrtgwt8TfpmbVpTKvA2oB8Nqey625CLN8bCN3TEbgx86Dsvr'
    golos_private = '5KekbiEKS4bwNptEtSawUygRb5sQ33P6EUZ6c4k4rEyQg7sARqW'
    golos_public = 'GLS6Tvw3apAGeHCUTWpf9DY4xvUoKwmuDatW7GV8ygkuZ8F6y4Yor'
    wallet_password = 'PW5JV8aRcMtZ8645EyCcZn9YfocvMFv52FP4VEuJQBWEzEwvS1d3v'


def jsonArg(a):
    return " '" + json.dumps(a) + "' "

def cleos(arguments):
    command=cleosCmd + arguments
    print('golos-boot-sequence.py:', command)
    if subprocess.call(command, shell=True):
        print("Command failed: ", command)
        return False
    return True

def importRootKeys():
    cleos('wallet import --private-key %s' % args.root_private)
    cleos('wallet import --private-key %s' % args.golos_private)
    pass

def removeRootKeys():
    cleos('wallet remove_key --password %s %s' % (args.wallet_password, args.root_public))
    cleos('wallet remove_key --password %s %s' % (args.wallet_password, args.golos_public))
    pass

def pushAction(code, action, actor, args):
    return cleos('push action %s %s %s -p %s' % (code, action, jsonArg(args), actor))

# --------------------- EOSIO functions ---------------------------------------

def createAuthority(keys, accounts):
    keys.sort()
    accounts.sort()
    keysList = []
    accountsList = []
    for k in keys:
        keysList.extend([{'weight':1,'key':k}])
    for a in accounts:
        d = a.split('@',2)
        if len(d) == 1:
            d.extend(['active'])
        accountsList.extend([{'weight':1,'permission':{'actor':d[0],'permission':d[1]}}])
    return {'threshold': 1, 'keys': keysList, 'accounts': accountsList, 'waits':[]}

def createAccount(creator, account, key):
    cleos('create account %s %s %s' % (creator, account, key))

def getAccount(account):
    cleos('get account %s' % account)

def updateAuth(account, permission, parent, keys, accounts):
    pushAction('eosio', 'updateauth', account, {
        'account': account,
        'permission': permission,
        'parent': parent,
        'auth': createAuthority(keys, accounts)
    })

def linkAuth(account, code, action, permission):
    cleos('set action permission %s %s %s %s -p %s'%(account, code, action, permission, account))

def transfer(sender, recipient, amount, memo=""):
    pushAction('eosio.token', 'transfer', sender, [sender, recipient, amount, memo])

# --------------------- GOLOS functions ---------------------------------------

def openVestingBalance(account):
    pushAction('gls.vesting', 'open', account, [account, args.vesting, account])

def openTokenBalance(account):
    pushAction('eosio.token', 'open', account, [account, args.token, account])

def issueToken(account, amount, memo=""):
    pushAction('eosio.token', 'issue', 'gls.publish@owner', [account, amount, memo])

def buyVesting(account, amount):
    issueToken(account, amount)
    transfer(account, 'gls.vesting', amount)   # buy vesting

def registerWitness(witness, key):
    pushAction('gls.ctrl', 'regwitness', witness, {
        'domain': args.token,
        'witness': witness,
        'key': key,
        'url': 'http://%s.witnesses.golos.io' % witness
    })

def voteWitness(voter, witness, weight):
    pushAction('gls.ctrl', 'votewitness', voter, [args.token, voter, witness, weight])

def createPost(author, permlink, category, header, body, *, beneficiaries=[]):
    pushAction('gls.publish', 'createmssg', author, 
        [author, permlink, "", category, beneficiaries, 0, False, header, body, 'ru', [], ''])

def createComment(author, permlink, pauthor, ppermlink, header, body, *, beneficiaries=[]):
    pushAction('gls.publish', 'createmssg', author,
        [author, permlink, pauthor, ppermlink, beneficiaries, 0, False, header, body, 'ru', [], ''])

def upvotePost(voter, author, permlink, weight):
    pushAction('gls.publish', 'upvote', voter, [voter, author, permlink, weight])

def downvotePost(voter, author, permlink, weight):
    pushAction('gls.publish', 'downvote', voter, [voter, author, permlink, weight])

def unvotePost(voter, author, permlink):
    pushAction('gls.publish', 'unvote', voter, [voter, author, permlink])

