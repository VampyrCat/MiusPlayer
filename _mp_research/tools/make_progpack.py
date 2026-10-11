"""Builds the two program definitions of the multiplayer drop-in folder.

The stock definitions are read from root\\programs.flatdata (never written) and patched copies are written to
Multiplayer_DropIn\\root\\programs\\. The engine prefers a loose root\\programs\\<name>.progpack over the packed
one, so copying the drop-in folder into a game adds multiplayer and deleting those files restores the stock game.

  tacgame (battles):   load basic_network.codelib, create the i_network object, call it first in every
                       simulation step and in the render pipeline
  opgame (menus, map): the stock definition already loads and creates it; only the render call is added
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from azcf import Azcf, pack_entries

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
pack = open(os.path.join(root, 'root', 'programs.flatdata'), 'rb').read()
out_dir = os.path.join(root, '_mp_research', 'Multiplayer_DropIn', 'root', 'programs')
os.makedirs(out_dir, exist_ok=True)


def stock(name):
    _, _, off, size = next(e for e in pack_entries(pack) if e[0] == name)
    return Azcf(pack[off:off + size])


def add_render_call(cfg):
    # just below the game's own cursor, so the cursor stays visible on top of the multiplayer menu
    rows = [r[0] for r in cfg.rows(cfg.find('r_main'))]
    cfg.insert_row('r_main', rows.index('i_cursor'), 0, {0: 'i_network'})


def save(cfg, name):
    path = os.path.join(out_dir, name + '.progpack')
    open(path, 'wb').write(cfg.b)
    check = Azcf(bytes(cfg.b))
    print('wrote', path, len(cfg.b), 'bytes')
    for table in ('codelib_list', 'obj_list', 'p_ctrl', 'r_main'):
        try:
            rows = [r[0] for r in check.rows(check.find(table))]
        except KeyError:
            continue
        print('   %-12s %s' % (table, 'i_network at %d of %d' % (rows.index('i_network'), len(rows)) if 'i_network' in rows
                               else [r for r in rows if 'network' in r] or 'no network entry'))


tac = stock('tacgame')
tac.append_string_row('codelib_list', 'basic_network.codelib')
tac.append_string_row('obj_list', 'i_network:network')
tac.insert_row('p_ctrl', 0, 0, {0: 'i_network'})
add_render_call(tac)
# keep simulating when the window is not focused (alt-tabbed players, two instances on one PC)
tac.set_field('i_window:window', 'lost_fwork', 1)
tac.set_field('i_loader:loader', 'lost_fwork', 1)
save(tac, 'tacgame')

op = stock('opgame')
add_render_call(op)
save(op, 'opgame')
