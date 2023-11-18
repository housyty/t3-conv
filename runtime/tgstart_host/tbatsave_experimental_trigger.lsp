(vl-load-com)

(setq *tbx-host-ready-path* "__TBX_HOST_READY__")
(setq *tbx-host-stop-path* "__TBX_HOST_STOP__")
(setq *tbx-host-bootstrap-path* "__TBX_HOST_BOOTSTRAP__")
(setq *tbx-trigger-log-path* "__TBX_TRIGGER_LOG__")
(setq *tbx-font-map-path* "__TBX_FONT_MAP__")
(setq *tbx-font-alt* "__TBX_FONT_ALT__")
(setq *tbx-font-search-path* "__TBX_FONT_SEARCH_PATH__")

(defun _tbx-has-text-p (value)
  (and value (/= value ""))
)

(defun _tbx-log (msg / logf)
  (setq logf (open *tbx-trigger-log-path* "a"))
  (if logf
    (progn
      (write-line (strcat "  [TBX] " msg) logf)
      (close logf)))
  (princ))

(defun _tbx-write-result (path lines / rf)
  (setq rf (open path "w"))
  (if rf
    (progn
      (foreach line lines
        (write-line line rf))
      (close rf)))
  (princ))

(defun _tbx-clear-file (path /)
  (if (and path (findfile path))
    (vl-file-delete path))
  (princ))

(defun _tbx-write-host-ready (/)
  (_tbx-write-result *tbx-host-ready-path* (list "READY"))
  (princ))

(defun _tbx-tch-kernal-loaded-p (/ loaded item lower)
  (setq loaded (arx))
  (while (and loaded (not lower))
    (setq item (car loaded))
    (if item
      (progn
        (setq lower (strcase item T))
        (if (not (wcmatch lower "*tch_kernal.arx*"))
          (setq lower nil))))
    (setq loaded (cdr loaded)))
  (and lower T))

(defun _tbx-wait-for-tch-kernal (/ i)
  (setq i 0)
  (while (and (< i 120) (not (_tbx-tch-kernal-loaded-p)))
    (setq i (1+ i))
    (if (= 0 (rem i 10))
      (_tbx-log "host ready deferred until tch_kernal.arx is loaded"))
    (vl-cmdf "._delay" 500))
  (if (_tbx-tch-kernal-loaded-p)
    (progn
      (_tbx-log "tch_kernal.arx loaded")
      T)
    (progn
      (_tbx-log "tch_kernal.arx wait timed out")
      nil)))

(defun _tbx-safe-setvar (name value / err)
  (_tbx-log (strcat "setvar " name " begin"))
  (setq err (vl-catch-all-apply 'setvar (list name value)))
  (if (vl-catch-all-error-p err)
    (_tbx-log (strcat "setvar " name " error: " (vl-catch-all-error-message err)))
    (_tbx-log (strcat "setvar " name " ok")))
  (princ))

(defun _tbx-prepare-env ()
  (_tbx-safe-setvar "filedia" 0)
  (_tbx-safe-setvar "cmddia" 0)
  (_tbx-safe-setvar "cmdecho" 0)
  (_tbx-safe-setvar "expert" 5)
  (if (_tbx-has-text-p *tbx-font-search-path*)
    (_tbx-safe-setvar "acadprefix"
      (strcat *tbx-font-search-path* ";" (getvar "acadprefix"))))
  (_tbx-safe-setvar "fontmap" *tbx-font-map-path*)
  (_tbx-safe-setvar "fontalt" *tbx-font-alt*)
  (_tbx-safe-setvar "proxynotice" 0)
  (_tbx-safe-setvar "proxyshow" 0)
  (_tbx-safe-setvar "proxywebsearch" 0)
  (_tbx-safe-setvar "xrefnotify" 0)
  (_tbx-safe-setvar "xloadctl" 0)
  (_tbx-safe-setvar "xrefctl" 0)
  (_tbx-safe-setvar "isavebak" 0)
  (if (boundp 'DownGradeSaveFlag) (setq DownGradeSaveFlag T))
  (if (boundp 'g_bSaveProxyGraph) (setq g_bSaveProxyGraph 1))
  (_tbx-log "env prepared")
  (princ))

(defun _tbx-host-stop-requested-p (/)
  (and (findfile *tbx-host-stop-path*) T)
)

(defun _tbx-start-host-loop (/)
  (if (_tbx-host-stop-requested-p)
    (progn
      (_tbx-log "host stop marker detected")
      (_tbx-clear-file *tbx-host-stop-path*)))
  (_tbx-clear-file *tbx-host-bootstrap-path*)
  (_tbx-prepare-env)
  (if (_tbx-wait-for-tch-kernal)
    (progn
      (_tbx-log "host ready")
      (_tbx-write-host-ready))
    (_tbx-log "host ready not written because tch_kernal.arx is missing"))
  (_tbx-log "host ready; returning control to AutoCAD")
  (princ))

(defun c:TBXHOSTSTOP ()
  (_tbx-clear-file *tbx-host-ready-path*)
  (princ))

(_tbx-start-host-loop)

(princ)
