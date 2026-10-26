import re
ui=open('ui.html').read(); core=open('dsp-core.js').read(); wk=open('worklet.js').read(); app=open('app.js').read()
assert '</script>' not in core+wk+app
scripts = ('\n<script id="ampex-core">\n' + core + '\n</script>\n'
           '<script id="ampex-worklet" type="text/plain">\n' + wk + '\n</script>\n'
           '<script>\n' + app + '\n</script>\n')
out = ui.replace('</body>', scripts + '</body>')
open('index.html','w').write(out)
print(len(out))
